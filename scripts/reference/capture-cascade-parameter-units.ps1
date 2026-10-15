# Read-only supplement: preserve raw parameter unit IDs, expressions and SI values.
# This does not submit an analysis, modify parameters, save or close the workspace.
param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false)
if(Test-Path -LiteralPath $OutputPath){throw 'Output already exists; preserve prior evidence'}
$interop='C:/Program Files/Keysight/SystemVue2023/Examples/RF Architecture Design/RF Design Kit/Scripting/Excel_to_RF/Interop.GENESYS.dll'
Add-Type -Path $interop
Add-Type -ReferencedAssemblies $interop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
public static class CascadeParameterUnits {
  public sealed class Variable {
    public string name,type;
    public object value;
    public int[] dimensions;
  }
  public sealed class Parameter {
    public string path;
    public Variable[] variables;
  }
  private static Parameter Read(GENESYS.IItem item,string path) {
    var variables=new List<Variable>();
    for(int i=0;i<item.GetVarCount();i++) {
      object value=item.GetVarValue(i);
      var variable=new Variable {name=item.GetVarName(i),type=item.GetVarType(i),value=value};
      var array=value as Array;
      if(array!=null) {
        if(array.Length>65536)throw new InvalidOperationException("Array bound exceeded");
        variable.dimensions=new int[array.Rank];
        for(int d=0;d<array.Rank;d++)variable.dimensions[d]=array.GetLength(d);
        var flat=new List<object>();
        foreach(object entry in array) {
          if(!(entry==null || entry is ValueType || entry is string))throw new InvalidOperationException("Unsupported array entry");
          flat.Add(entry);
        }
        variable.value=flat.ToArray();
      } else if(!(value==null || value is ValueType || value is string)) {
        throw new InvalidOperationException("Unsupported parameter metadata");
      }
      variables.Add(variable);
    }
    return new Parameter {path=path,variables=variables.ToArray()};
  }
  public static object Run() {
    object active=Marshal.GetActiveObject("Genesys.Application");
    try {
      var app=(GENESYS.Application)active;var manager=app.Manager;
      try {
        if(manager.GetWorkspaceCount()!=1 || manager.GetErrors()!="")throw new InvalidOperationException("Unexpected workspaces/errors");
        var workspace=manager.GetWorkspaceByIndex(0);
        try {
          var root=(GENESYS.IItem)workspace;
          if(root.GetName()!="RFModel_CascadeIntermods")throw new InvalidOperationException("Unexpected workspace");
          if(!Convert.ToBoolean(workspace.GetType().InvokeMember("IsAnalysisDone",BindingFlags.InvokeMethod,null,workspace,new object[0])))throw new InvalidOperationException("Analysis is active");
          var results=new List<Parameter>();
          var designs=root.GetItemByName("Designs");
          try {
            var design=designs.GetItemByName("Design3");
            try {
              var parts=design.GetItemByName("PartList");
              try {
                foreach(string name in new[]{"RFAmp1","RFAmp2","Source","Port_2"}) {
                  var part=parts.GetItemByName(name);
                  try {
                    var parameters=part.GetItemByName("ParamSet");
                    try {
                      for(int i=0;i<parameters.GetItemCount();i++) {
                        var parameter=parameters.GetItemByIndex(i);
                        try{results.Add(Read(parameter,"RFModel_CascadeIntermods/Designs/Design3/PartList/"+name+"/ParamSet/"+parameter.GetName()));}
                        finally{Marshal.ReleaseComObject(parameter);}
                      }
                    }finally{Marshal.ReleaseComObject(parameters);}
                  }finally{Marshal.ReleaseComObject(part);}
                }
              }finally{Marshal.ReleaseComObject(parts);}
            }finally{Marshal.ReleaseComObject(design);}
          }finally{Marshal.ReleaseComObject(designs);}
          if(manager.GetErrors()!="")throw new InvalidOperationException("Error during readback");
          return new {utc=DateTime.UtcNow.ToString("o"),parameters=results};
        }finally{Marshal.ReleaseComObject(workspace);}
      }finally{Marshal.ReleaseComObject(manager);}
    }finally{Marshal.ReleaseComObject(active);}
  }
}
'@
$result=[ordered]@{
  schema_version=1
  classification='read_only_parameter_units_supplement_no_analysis'
  systemvue_version=[Diagnostics.FileVersionInfo]::GetVersionInfo('C:/Program Files/Keysight/SystemVue2023/Bin/SystemVue.exe').FileVersion
  source_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
  readback=[CascadeParameterUnits]::Run()
}
$stream=[IO.File]::Open($OutputPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try{$bytes=[Text.Encoding]::UTF8.GetBytes(($result|ConvertTo-Json -Depth 14));$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
Write-Output ('Read '+$result.readback.parameters.Count+' parameters; no analysis submitted.')
