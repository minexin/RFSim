# Submit exactly one explicitly named analysis in the owned frozen RES fixture.
# Windows PowerShell 5.1 only. No retries, Manager.Update, or error-log clearing.
param(
 [Parameter(Mandatory=$true)][string]$ExpectedWorkspace,
 [Parameter(Mandatory=$true)][string]$OutputStem,
 [Parameter(Mandatory=$true)][ValidateSet('RES_Linear','RES_RFSystem')][string]$AnalysisName,
 [Parameter(Mandatory=$true)][string]$PlanPath
)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1){throw 'Use Windows PowerShell 5.1'}
$planHash=(Get-FileHash -LiteralPath $PlanPath -Algorithm SHA256).Hash.ToLowerInvariant()
if($planHash -ne '4000c4549f5ed0a129f1f5ccf1069d4ef4c36be6da9c5de04d86b441f9d4a30e'){throw 'Frozen plan hash mismatch'}
if($OutputStem -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+[\\/])'){throw 'OutputStem must be an absolute drive or UNC path'}
$OutputStem=[IO.Path]::GetFullPath($OutputStem)
if(-not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($OutputStem))){throw 'Output directory missing'}
foreach($suffix in @('.json','-input.wsv','-result.wsv','-phases.log','-attempt.json')){
 if(Test-Path -LiteralPath ($OutputStem+$suffix)){throw 'Evidence exists; never resubmit this attempt'}
}
$attempt=[ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');expected_workspace=$ExpectedWorkspace;analysis=$AnalysisName;plan_sha256=$planHash;collector_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant();status='reserved_not_submitted'}
$stream=[IO.File]::Open($OutputStem+'-attempt.json',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($attempt|ConvertTo-Json));$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
$interop='C:/Program Files/Keysight/SystemVue2023/Examples/RF Architecture Design/RF Design Kit/Scripting/Excel_to_RF/Interop.GENESYS.dll'
Add-Type -Path $interop
Add-Type -ReferencedAssemblies $interop -TypeDefinition @'
using System;using System.Collections.Generic;using System.Runtime.InteropServices;using System.Reflection;using System.IO;
public static class RunRESAnalysis {
 static object Call(object o,string n,params object[] a){return o.GetType().InvokeMember(n,BindingFlags.InvokeMethod,null,o,a);}
 static void Release(object o){if(o!=null&&Marshal.IsComObject(o))Marshal.ReleaseComObject(o);}
 static void Phase(string stem,string message){File.AppendAllText(stem+"-phases.log",DateTime.UtcNow.ToString("o")+" "+message+Environment.NewLine);Console.Error.WriteLine(message);}
 static object Parameter(GENESYS.IItem parent,string name){var p=parent.GetItemByName(name);try{return Call(p,"GetValue");}finally{Release(p);}}
 static void Number(object actual,double expected,string name){
  if(actual==null||actual is Array)throw new Exception("Preflight scalar missing: "+name);
  double value=Convert.ToDouble(actual);
  if(Double.IsNaN(value)||Double.IsInfinity(value)||Math.Abs(value-expected)>1e-12*Math.Max(1,Math.Abs(expected)))throw new Exception("Preflight value differs: "+name);
 }
 static void Check(GENESYS.IItem parent,string name,double expected){Number(Parameter(parent,name),expected,parent.GetName()+"/"+name);}
 static void Property(GENESYS.IItem parent,string name,double expected){
  for(int i=0;i<parent.GetVarCount();i++)if(parent.GetVarXMLName(i)==name){Number(parent.GetVarValue(i),expected,parent.GetName()+"/"+name);return;}
  throw new Exception("Preflight property missing: "+name);
 }
 public static object Run(string expected,string stem,string analysisName){
  object active=null;GENESYS.Manager m=null;GENESYS.Workspace w=null;GENESYS.IItem designs=null;GENESYS.IItem analysis=null;
  string started=null,returned=null,exception=null;int requests=0;bool done=false;string errorsBefore=null,errorsAfter=null;
  string input=null,result=null,partSnapshot=null,dataset=Path.GetFileName(stem).Replace('-','_')+"_Data";
  try{
   active=Marshal.GetActiveObject("Genesys.Application");m=((GENESYS.Application)active).Manager;
   if(m.GetWorkspaceCount()!=1)throw new Exception("Require exactly one owned workspace");
   w=m.GetWorkspaceByIndex(0);var root=(GENESYS.IItem)w;
   if(root.GetName()!=expected||!Convert.ToBoolean(Call(w,"IsAnalysisDone")))throw new Exception("Unexpected workspace or active analysis");
   errorsBefore=m.GetErrors();if(errorsBefore!="")throw new Exception("Existing application errors; no analysis submitted");
   designs=root.GetItemByName("Designs");var design=designs.GetItemByName("RES_Shared");
   try{var parts=design.GetItemByName("PartList");try{
    string[] names={"Source","R1","R2","Load"};string[] models={"MultiSource","RES","RES","*OUT"};
    for(int i=0;i<names.Length;i++){var part=parts.GetItemByName(names[i]);try{
     if(!part.ToXML().Contains("<Model>"+models[i]+"</Model>"))throw new Exception("Wrong model "+names[i]);
     var ps=part.GetItemByName("ParamSet");try{
      if(names[i]=="Source"){
       Check(ps,"Freq",1e8);Check(ps,"BW",1);Check(ps,"Pwr",0.001);Check(ps,"Phase",0);
       Check(ps,"PORT",1);Check(ps,"R",50);Check(ps,"ZO",50);Check(ps,"Enable",1);Check(ps,"SrcType",0);
       Check(ps,"MultiCarrier",0);Check(ps,"EnablePN",0);Check(ps,"Desired",1);Check(ps,"POff",1);Check(ps,"PhOff",0);
      }else if(names[i]=="Load"){Check(ps,"PORT",2);Check(ps,"ZO",50);}else Check(ps,"R",50);
     }finally{Release(ps);}
    }finally{Release(part);}}
    partSnapshot=parts.ToXML();
   }finally{Release(parts);}}finally{Release(design);}
   analysis=designs.GetItemByName(analysisName);
   for(int i=0;i<designs.GetItemCount();i++){var child=designs.GetItemByIndex(i);try{if(child.GetName()==dataset)throw new Exception("Dataset already exists; no overwrite permitted");}finally{Release(child);}}
   if(Convert.ToString(Parameter(analysis,"DesignName"))!="RES_Shared")throw new Exception("Wrong shared design");
   Property(analysis,"AutoRecalc",0);Property(analysis,"CalcNoise",0);
   Check(analysis,"Temperature",26.85);
   if(analysisName=="RES_Linear"){Check(analysis,"Start",1e8);Check(analysis,"Stop",1e8);Check(analysis,"NumPoints",1);}
   else{Check(analysis,"ChanBW",1000);Check(analysis,"NominalZ0",50);Check(analysis,"MaxOrder",3);Check(analysis,"UseSpecReduction",0);Property(analysis,"CalcHarmonics",0);Property(analysis,"CalcIntermods",0);Property(analysis,"CalcPhaseNoise",0);}
   Call(analysis,"SetDataName",dataset);w.SaveAs(stem+"-input.wsv");input=root.ToXML();
   Phase(stem,"validated-candidate-input-saved; semantic identity gates remain for result interpretation");
   Call(analysis,"ClearModelCache");
   started=DateTime.UtcNow.ToString("o");Phase(stem,"submit-one-RunAnalysis");requests=1;
   Call(analysis,"RunAnalysis");returned=DateTime.UtcNow.ToString("o");
   done=Convert.ToBoolean(Call(w,"IsAnalysisDone"));errorsAfter=m.GetErrors();
   Phase(stem,done?"analysis-done-read-existing-results":"analysis-pending-do-not-resubmit");
   if(done){result=root.ToXML();w.SaveAs(stem+"-result.wsv");}
  }catch(Exception e){exception=e.ToString();try{if(m!=null)errorsAfter=m.GetErrors();}catch{}Phase(stem,"exception-retained-no-retry");}
  finally{Release(analysis);Release(designs);Release(w);Release(m);Release(active);}
  return new{analysis=analysisName,expected_workspace=expected,dataset=dataset,analysis_requests=requests,
   submitted_utc=started,returned_utc=returned,captured_utc=DateTime.UtcNow.ToString("o"),analysis_done=done,
   exception=exception,errors_before=errorsBefore,errors_after=errorsAfter,input_xml=input,result_xml=result,part_snapshot=partSnapshot,
   acceptance="unscored; settings, dataset, array, unit, reference-plane and resolved-path gates are separate"};
 }
}
'@
$r=[RunRESAnalysis]::Run($ExpectedWorkspace,$OutputStem,$AnalysisName)
$record=[ordered]@{plan_sha256=$planHash;script_sha256=$attempt.collector_sha256;interop_sha256=(Get-FileHash -LiteralPath $interop -Algorithm SHA256).Hash.ToLowerInvariant();run=$r}
$json=$record|ConvertTo-Json -Depth 40
$stream=[IO.File]::Open($OutputStem+'.json',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes($json);$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
$r|Select-Object analysis,dataset,analysis_requests,submitted_utc,returned_utc,analysis_done,exception,errors_before,errors_after|ConvertTo-Json
if($r.exception -or $r.errors_after){exit 1}
