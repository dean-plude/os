#!/bin/sh
# make_service_package.sh — build svc.msi: novasvc.exe (svc.c) installed as
# the NovaTestSvc service, started on install, stopped and deleted on
# uninstall (ServiceInstall, ServiceControl), with msitools:
#   x86_64-w64-mingw32-gcc -O2 -o novasvc.exe svc.c -ladvapi32
#   sh make_service_package.sh
set -e
rm -f svc.msi svc.cab
cp novasvc.exe svcexe            # cabinet entries are named by File key
gcab -c svc.cab svcexe
rm -f svcexe
q() { msibuild svc.msi -q "$1"; }
msibuild svc.msi -s "Nova Service Test" "NovaOS" "x64;1033" "{5E2B0C1A-7D3E-4F60-9A11-0123456789AB}"
q "CREATE TABLE \`Property\` (\`Property\` CHAR(72) NOT NULL, \`Value\` CHAR(0) NOT NULL LOCALIZABLE PRIMARY KEY \`Property\`)"
for p in "ProductName|Nova Service Test" "ProductVersion|1.0.0" "Manufacturer|NovaOS Project" \
         "ProductCode|{5E2B0C1A-7D3E-4F60-9A11-0123456789AB}" "UpgradeCode|{5E2B0C1A-7D3E-4F60-9A11-0000000000AB}" \
         "ProductLanguage|1033" "ALLUSERS|1"; do
  q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('${p%%|*}', '${p#*|}')"
done
q "CREATE TABLE \`Directory\` (\`Directory\` CHAR(72) NOT NULL, \`Directory_Parent\` CHAR(72), \`DefaultDir\` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY \`Directory\`)"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('TARGETDIR', '', 'SourceDir')"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('ProgramFiles64Folder', 'TARGETDIR', 'PFiles')"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('INSTALLDIR', 'ProgramFiles64Folder', 'NovaSvc')"
q "CREATE TABLE \`Component\` (\`Component\` CHAR(72) NOT NULL, \`ComponentId\` CHAR(38), \`Directory_\` CHAR(72) NOT NULL, \`Attributes\` SHORT NOT NULL, \`Condition\` CHAR(255), \`KeyPath\` CHAR(72) PRIMARY KEY \`Component\`)"
q "INSERT INTO \`Component\` (\`Component\`, \`ComponentId\`, \`Directory_\`, \`Attributes\`, \`Condition\`, \`KeyPath\`) VALUES ('SvcExe', '{B0000000-0000-4000-8000-000000000001}', 'INSTALLDIR', 256, '', 'svcexe')"
q "CREATE TABLE \`Feature\` (\`Feature\` CHAR(38) NOT NULL, \`Feature_Parent\` CHAR(38), \`Title\` CHAR(64) LOCALIZABLE, \`Description\` CHAR(255) LOCALIZABLE, \`Display\` SHORT, \`Level\` SHORT NOT NULL, \`Directory_\` CHAR(72), \`Attributes\` SHORT NOT NULL PRIMARY KEY \`Feature\`)"
q "INSERT INTO \`Feature\` (\`Feature\`, \`Feature_Parent\`, \`Title\`, \`Description\`, \`Display\`, \`Level\`, \`Directory_\`, \`Attributes\`) VALUES ('Complete', '', 'Service', 'The service', 1, 1, 'INSTALLDIR', 0)"
q "CREATE TABLE \`FeatureComponents\` (\`Feature_\` CHAR(38) NOT NULL, \`Component_\` CHAR(72) NOT NULL PRIMARY KEY \`Feature_\`, \`Component_\`)"
q "INSERT INTO \`FeatureComponents\` (\`Feature_\`, \`Component_\`) VALUES ('Complete', 'SvcExe')"
q "CREATE TABLE \`File\` (\`File\` CHAR(72) NOT NULL, \`Component_\` CHAR(72) NOT NULL, \`FileName\` CHAR(255) NOT NULL LOCALIZABLE, \`FileSize\` LONG NOT NULL, \`Version\` CHAR(72), \`Language\` CHAR(20), \`Attributes\` SHORT, \`Sequence\` SHORT NOT NULL PRIMARY KEY \`File\`)"
q "INSERT INTO \`File\` (\`File\`, \`Component_\`, \`FileName\`, \`FileSize\`, \`Version\`, \`Language\`, \`Attributes\`, \`Sequence\`) VALUES ('svcexe', 'SvcExe', 'novasvc.exe', $(stat -c %s novasvc.exe), '', '', 512, 1)"
q "CREATE TABLE \`Media\` (\`DiskId\` SHORT NOT NULL, \`LastSequence\` SHORT NOT NULL, \`DiskPrompt\` CHAR(64) LOCALIZABLE, \`Cabinet\` CHAR(255), \`VolumeLabel\` CHAR(32), \`Source\` CHAR(72) PRIMARY KEY \`DiskId\`)"
q "INSERT INTO \`Media\` (\`DiskId\`, \`LastSequence\`, \`DiskPrompt\`, \`Cabinet\`, \`VolumeLabel\`, \`Source\`) VALUES (1, 1, '', '#svc.cab', '', '')"
q "CREATE TABLE \`ServiceInstall\` (\`ServiceInstall\` CHAR(72) NOT NULL, \`Name\` CHAR(255) NOT NULL, \`DisplayName\` CHAR(255) LOCALIZABLE, \`ServiceType\` LONG NOT NULL, \`StartType\` LONG NOT NULL, \`ErrorControl\` LONG NOT NULL, \`LoadOrderGroup\` CHAR(255), \`Dependencies\` CHAR(255), \`StartName\` CHAR(255), \`Password\` CHAR(255), \`Arguments\` CHAR(255), \`Component_\` CHAR(72) NOT NULL, \`Description\` CHAR(255) LOCALIZABLE PRIMARY KEY \`ServiceInstall\`)"
q "INSERT INTO \`ServiceInstall\` (\`ServiceInstall\`, \`Name\`, \`DisplayName\`, \`ServiceType\`, \`StartType\`, \`ErrorControl\`, \`LoadOrderGroup\`, \`Dependencies\`, \`StartName\`, \`Password\`, \`Arguments\`, \`Component_\`, \`Description\`) VALUES ('si1', 'NovaTestSvc', 'Nova Test Service', 16, 2, 1, '', '', '', '', '', 'SvcExe', 'Writes its state to a file')"
q "CREATE TABLE \`ServiceControl\` (\`ServiceControl\` CHAR(72) NOT NULL, \`Name\` CHAR(255) NOT NULL LOCALIZABLE, \`Event\` SHORT NOT NULL, \`Arguments\` CHAR(255) LOCALIZABLE, \`Wait\` SHORT, \`Component_\` CHAR(72) NOT NULL PRIMARY KEY \`ServiceControl\`)"
q "INSERT INTO \`ServiceControl\` (\`ServiceControl\`, \`Name\`, \`Event\`, \`Arguments\`, \`Wait\`, \`Component_\`) VALUES ('sc1', 'NovaTestSvc', 161, '', 1, 'SvcExe')"
q "CREATE TABLE \`InstallExecuteSequence\` (\`Action\` CHAR(72) NOT NULL, \`Condition\` CHAR(255), \`Sequence\` SHORT PRIMARY KEY \`Action\`)"
for a in "CostInitialize 800" "FileCost 900" "CostFinalize 1000" "InstallValidate 1400" "InstallInitialize 1500" \
         "ProcessComponents 1600" "StopServices 1900" "DeleteServices 2000" "RemoveFiles 3500" "InstallFiles 4000" \
         "InstallServices 5800" "StartServices 5900" "RegisterProduct 6100" "InstallFinalize 6600"; do
  set -- $a; q "INSERT INTO \`InstallExecuteSequence\` (\`Action\`, \`Condition\`, \`Sequence\`) VALUES ('$1', '', $2)"
done
msibuild svc.msi -a svc.cab svc.cab
