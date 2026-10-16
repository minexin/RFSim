# Windows PowerShell 5.1. Creates only the owned RES workspace in an empty app.
param([Parameter(Mandatory=$true)][string]$EvidenceDirectory,
 [Parameter(Mandatory=$true)][string]$PlanPath,
 [string]$InteropPath='C:/Program Files/Keysight/SystemVue2023/Examples/RF Architecture Design/RF Design Kit/Scripting/Excel_to_RF/Interop.GENESYS.dll')
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1){throw 'Use Windows PowerShell 5.1'}
$planHash=(Get-FileHash -LiteralPath $PlanPath -Algorithm SHA256).Hash.ToLowerInvariant()
if($planHash -ne '4000c4549f5ed0a129f1f5ccf1069d4ef4c36be6da9c5de04d86b441f9d4a30e'){throw 'Frozen plan hash mismatch'}
$EvidenceDirectory=(Resolve-Path -LiteralPath $EvidenceDirectory).Path
$inputPath=Join-Path $EvidenceDirectory 'res-input-uninitialized.wsv'
$receiptPath=Join-Path $EvidenceDirectory 'input-generation.json'
$receipt=Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json
$inputHash=(Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash.ToLowerInvariant()
if($receipt.status -ne 'uninitialized_input_not_simulated' -or
   $receipt.plan_id -ne 'systemvue-linear-rfsystem-minimal-20261016-v1' -or
   $receipt.plan_sha256 -ne $planHash -or $receipt.input_sha256 -ne $inputHash -or
   $null -eq $receipt.analysis_requests -or $receipt.analysis_requests -ne 0){throw 'Generated input receipt/hash mismatch'}
foreach($name in @('res-initialized.wsv','initialization.json','initialization-phases.log','initialization-attempt.json')) {
 if(Test-Path -LiteralPath (Join-Path $EvidenceDirectory $name)){throw "Evidence exists: $name"}
}
# Reserve this initialization before any application access. A failed attempt
# remains reserved; investigating it must not silently reinitialize the input.
$attempt=[ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');status='reserved_not_initialized';plan_sha256=$planHash;input_sha256=$inputHash;input_receipt_sha256=(Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash.ToLowerInvariant();initializer_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant();interop_sha256=(Get-FileHash -LiteralPath $InteropPath -Algorithm SHA256).Hash.ToLowerInvariant()}
$stream=[IO.File]::Open((Join-Path $EvidenceDirectory 'initialization-attempt.json'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($attempt|ConvertTo-Json));$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
Add-Type -Path $InteropPath
Add-Type -ReferencedAssemblies $InteropPath -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
using System.Reflection; using System.IO;
public static class RESInitialize {
 static object Call(object o,string name,params object[] args){return o.GetType().InvokeMember(name,BindingFlags.InvokeMethod,null,o,args);}
 static void Phase(string p,string s){File.AppendAllText(p+"/initialization-phases.log",DateTime.UtcNow.ToString("o")+" "+s+Environment.NewLine);Console.Error.WriteLine(s);}
 static object Set(GENESYS.IItem part,string name,string value){
  var ps=part.GetItemByName("ParamSet");try{var p=ps.GetItemByName(name);try{
   Call(p,"Set",value);return new{part=part.GetName(),parameter=name,expression=value,
    display=Call(p,"Get"),evaluated_mks=Call(p,"GetValue"),xml=p.ToXML()};
  }finally{Marshal.ReleaseComObject(p);}}finally{Marshal.ReleaseComObject(ps);}
 }
 public static object Run(string dir){
  object active=Marshal.GetActiveObject("Genesys.Application");try{
   var app=(GENESYS.Application)active;var manager=app.Manager;try{
    if(manager.GetWorkspaceCount()!=0)throw new Exception("Require empty application; no workspace changed");
    string errorsBefore=manager.GetErrors();
    Phase(dir,"open-owned-input-no-analysis");manager.FileOpen(dir+"/res-input-uninitialized.wsv");
    if(manager.GetWorkspaceCount()!=1)throw new Exception("Expected exactly one owned workspace after open");
    var workspace=manager.GetWorkspaceByIndex(0);try{
     var root=(GENESYS.IItem)workspace;
     if(root.GetName()!="res-input-uninitialized")throw new Exception("Unexpected workspace; no initialization submitted");
     if(!Convert.ToBoolean(Call(workspace,"IsAnalysisDone")))throw new Exception("Application reports analysis running");
     var operations=new List<object>();string failure=null;
     try{
      var designs=root.GetItemByName("Designs");try{
       var design=designs.GetItemByName("RES_Shared");try{
        var parts=design.GetItemByName("PartList");try{
         string[] names={"Source","R1","R2","Load"};string[] models={"MultiSource","RES","RES","*OUT"};
         for(int i=0;i<names.Length;i++){
          Phase(dir,"initialize-"+names[i]);var part=parts.GetItemByName(names[i]);try{
           Call(part,"ChangeModel",models[i]);
           if(names[i]=="Source"){
            string[] ns={"Name","Enable","SrcType","MultiCarrier","EnablePN","Desired","Freq","BW","Pwr","Phase","RefClk","R","ZO","PORT","POff","PhOff"};
            string[] vs={"=[\"RES_CW\"]","[1]","[0]","[0]","[0]","[1]","[100]","[0.000001]","[0]","[0]","=[\"\"]","50","50","1","0","0"};
            for(int j=0;j<ns.Length;j++)operations.Add(Set(part,ns[j],vs[j]));
           }else if(names[i]=="Load"){
            operations.Add(Set(part,"PORT","2"));operations.Add(Set(part,"ZO","50"));
           }else operations.Add(Set(part,"R","50"));
          }finally{Marshal.ReleaseComObject(part);}
         }
        }finally{Marshal.ReleaseComObject(parts);}
       }finally{Marshal.ReleaseComObject(design);}
      }finally{Marshal.ReleaseComObject(designs);}
     }catch(Exception e){failure=e.ToString();}
     if(File.Exists(dir+"/res-initialized.wsv"))throw new Exception("Initialized workspace exists; no overwrite permitted");
     Phase(dir,"save-initialized-input-no-analysis");workspace.SaveAs(dir+"/res-initialized.wsv");
     return new{utc=DateTime.UtcNow.ToString("o"),analysis_requests=0,workspace=root.GetName(),
      failure=failure,errors_before=errorsBefore,errors=manager.GetErrors(),operations=operations,xml=root.ToXML()};
    }finally{Marshal.ReleaseComObject(workspace);}
   }finally{Marshal.ReleaseComObject(manager);}
  }finally{Marshal.ReleaseComObject(active);}
 }
}
'@
$record=[RESInitialize]::Run($EvidenceDirectory)
$envelope=[ordered]@{plan_sha256=$planHash;input_sha256=$inputHash;input_receipt_sha256=$attempt.input_receipt_sha256;initializer_sha256=$attempt.initializer_sha256;interop_sha256=$attempt.interop_sha256;initialization=$record}
$stream=[IO.File]::Open((Join-Path $EvidenceDirectory 'initialization.json'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($envelope|ConvertTo-Json -Depth 32));$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
[pscustomobject]@{workspace=$record.workspace;failure=$record.failure;errors=$record.errors;analysis_requests=$record.analysis_requests;operations=$record.operations}|ConvertTo-Json -Depth 10
if($record.failure){exit 1}
