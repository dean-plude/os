# .NET test programs

`culturetest.cs` checks .NET's globalization on NovaOS: German and Japanese
numbers, currencies, dates, names and string comparison, which .NET does
through ICU (`C:\Windows\System32\icu.dll`).  It prints one line per check
and `culturetest: ok` when every value matches what Windows 10 prints.  The
nightly app corpus (`tools/appcorpus.py`, the **.NET** entry) runs it.

`culturetest.dll` is the compiled program, kept here so the corpus needs no
.NET SDK.  To rebuild it after changing the source, with any .NET 10 SDK:

```sh
csc -nologo -optimize -deterministic -out:culturetest.dll \
    -r:<dotnet>/packs/Microsoft.NETCore.App.Ref/10.0.x/ref/net10.0/*.dll culturetest.cs
```

(or `dotnet csc.dll ...` with the compiler from the
`Microsoft.Net.Compilers.Toolset` NuGet package and the reference
assemblies from `Microsoft.NETCore.App.Ref`).
