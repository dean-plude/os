#!/bin/sh
# make_package.sh — build test.msi (an MSZIP cabinet with hello.exe and a
# readme, directories, registry values, custom actions) with msitools:
#   apt install msitools gcab; cd DIR-with-hello.exe-and-readme.txt; sh make_package.sh
set -e
rm -f test.msi
q() { msibuild test.msi -q "$1"; }
msibuild test.msi -s "MSI Test App" "NovaOS" "x64;1033" "{9A1F9E0C-3C7B-4B5C-9E7A-1234567890AB}"
q "CREATE TABLE \`Property\` (\`Property\` CHAR(72) NOT NULL, \`Value\` CHAR(0) NOT NULL LOCALIZABLE PRIMARY KEY \`Property\`)"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('ProductName', 'MSI Test App')"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('ProductVersion', '1.2.3')"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('Manufacturer', 'NovaOS Project')"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('ProductCode', '{9A1F9E0C-3C7B-4B5C-9E7A-1234567890AB}')"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('UpgradeCode', '{1B2C3D4E-0000-4000-8000-000000000001}')"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('ProductLanguage', '1033')"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('ALLUSERS', '1')"
q "INSERT INTO \`Property\` (\`Property\`, \`Value\`) VALUES ('ARPHELPLINK', 'https://example.org/[ProductName]')"
q "CREATE TABLE \`Directory\` (\`Directory\` CHAR(72) NOT NULL, \`Directory_Parent\` CHAR(72), \`DefaultDir\` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY \`Directory\`)"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('TARGETDIR', '', 'SourceDir')"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('ProgramFiles64Folder', 'TARGETDIR', 'PFiles')"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('INSTALLDIR', 'ProgramFiles64Folder', 'MSITES~1|MSI Test App')"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('DocsDir', 'INSTALLDIR', 'docs')"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('ProgramMenuFolder', 'TARGETDIR', 'PMenu')"
q "INSERT INTO \`Directory\` (\`Directory\`, \`Directory_Parent\`, \`DefaultDir\`) VALUES ('MenuDir', 'ProgramMenuFolder', 'MSITES~1|MSI Test App')"
q "CREATE TABLE \`Component\` (\`Component\` CHAR(72) NOT NULL, \`ComponentId\` CHAR(38), \`Directory_\` CHAR(72) NOT NULL, \`Attributes\` SHORT NOT NULL, \`Condition\` CHAR(255), \`KeyPath\` CHAR(72) PRIMARY KEY \`Component\`)"
q "INSERT INTO \`Component\` (\`Component\`, \`ComponentId\`, \`Directory_\`, \`Attributes\`, \`Condition\`, \`KeyPath\`) VALUES ('MainExe', '{A0000000-0000-4000-8000-000000000001}', 'INSTALLDIR', 256, '', 'helloexe')"
q "INSERT INTO \`Component\` (\`Component\`, \`ComponentId\`, \`Directory_\`, \`Attributes\`, \`Condition\`, \`KeyPath\`) VALUES ('Docs', '{A0000000-0000-4000-8000-000000000002}', 'DocsDir', 256, '', 'readmetxt')"
q "INSERT INTO \`Component\` (\`Component\`, \`ComponentId\`, \`Directory_\`, \`Attributes\`, \`Condition\`, \`KeyPath\`) VALUES ('RegComp', '{A0000000-0000-4000-8000-000000000003}', 'INSTALLDIR', 260, '', 'reg1')"
q "INSERT INTO \`Component\` (\`Component\`, \`ComponentId\`, \`Directory_\`, \`Attributes\`, \`Attributes\`, \`Condition\`, \`KeyPath\`) VALUES ('NeverComp', '{A0000000-0000-4000-8000-000000000004}', 'INSTALLDIR', 256, 256, 'NOTSET', 'readmetxt2')" || true
q "CREATE TABLE \`Feature\` (\`Feature\` CHAR(38) NOT NULL, \`Feature_Parent\` CHAR(38), \`Title\` CHAR(64) LOCALIZABLE, \`Description\` CHAR(255) LOCALIZABLE, \`Display\` SHORT, \`Level\` SHORT NOT NULL, \`Directory_\` CHAR(72), \`Attributes\` SHORT NOT NULL PRIMARY KEY \`Feature\`)"
q "INSERT INTO \`Feature\` (\`Feature\`, \`Feature_Parent\`, \`Title\`, \`Description\`, \`Display\`, \`Level\`, \`Directory_\`, \`Attributes\`) VALUES ('Complete', '', 'Everything', 'All of it', 2, 1, 'INSTALLDIR', 0)"
q "INSERT INTO \`Feature\` (\`Feature\`, \`Feature_Parent\`, \`Title\`, \`Description\`, \`Display\`, \`Level\`, \`Directory_\`, \`Attributes\`) VALUES ('Hidden', 'Complete', 'Not installed', 'Level 0', 0, 0, 'INSTALLDIR', 0)"
q "CREATE TABLE \`FeatureComponents\` (\`Feature_\` CHAR(38) NOT NULL, \`Component_\` CHAR(72) NOT NULL PRIMARY KEY \`Feature_\`, \`Component_\`)"
q "INSERT INTO \`FeatureComponents\` (\`Feature_\`, \`Component_\`) VALUES ('Complete', 'MainExe')"
q "INSERT INTO \`FeatureComponents\` (\`Feature_\`, \`Component_\`) VALUES ('Complete', 'Docs')"
q "INSERT INTO \`FeatureComponents\` (\`Feature_\`, \`Component_\`) VALUES ('Complete', 'RegComp')"
q "CREATE TABLE \`File\` (\`File\` CHAR(72) NOT NULL, \`Component_\` CHAR(72) NOT NULL, \`FileName\` CHAR(255) NOT NULL LOCALIZABLE, \`FileSize\` LONG NOT NULL, \`Version\` CHAR(72), \`Language\` CHAR(20), \`Attributes\` SHORT, \`Sequence\` SHORT NOT NULL PRIMARY KEY \`File\`)"
q "INSERT INTO \`File\` (\`File\`, \`Component_\`, \`FileName\`, \`FileSize\`, \`Version\`, \`Language\`, \`Attributes\`, \`Sequence\`) VALUES ('helloexe', 'MainExe', 'hello.exe', $(stat -c %s hello.exe), '', '', 512, 1)"
q "INSERT INTO \`File\` (\`File\`, \`Component_\`, \`FileName\`, \`FileSize\`, \`Version\`, \`Language\`, \`Attributes\`, \`Sequence\`) VALUES ('readmetxt', 'Docs', 'README~1.TXT|readme long name.txt', $(stat -c %s readme.txt), '', '', 512, 2)"
q "CREATE TABLE \`Media\` (\`DiskId\` SHORT NOT NULL, \`LastSequence\` SHORT NOT NULL, \`DiskPrompt\` CHAR(64) LOCALIZABLE, \`Cabinet\` CHAR(255), \`VolumeLabel\` CHAR(32), \`Source\` CHAR(72) PRIMARY KEY \`DiskId\`)"
q "INSERT INTO \`Media\` (\`DiskId\`, \`LastSequence\`, \`DiskPrompt\`, \`Cabinet\`, \`VolumeLabel\`, \`Source\`) VALUES (1, 2, '', '#test.cab', '', '')"
q "CREATE TABLE \`Registry\` (\`Registry\` CHAR(72) NOT NULL, \`Root\` SHORT NOT NULL, \`Key\` CHAR(255) NOT NULL LOCALIZABLE, \`Name\` CHAR(255) LOCALIZABLE, \`Value\` CHAR(0) LOCALIZABLE, \`Component_\` CHAR(72) NOT NULL PRIMARY KEY \`Registry\`)"
q "INSERT INTO \`Registry\` (\`Registry\`, \`Root\`, \`Key\`, \`Name\`, \`Value\`, \`Component_\`) VALUES ('reg1', 2, 'Software\\NovaOS\\MSITest', 'InstallDir', '[INSTALLDIR]', 'RegComp')"
q "INSERT INTO \`Registry\` (\`Registry\`, \`Root\`, \`Key\`, \`Name\`, \`Value\`, \`Component_\`) VALUES ('reg2', 2, 'Software\\NovaOS\\MSITest', 'Version', '#3', 'RegComp')"
q "INSERT INTO \`Registry\` (\`Registry\`, \`Root\`, \`Key\`, \`Name\`, \`Value\`, \`Component_\`) VALUES ('reg3', 1, 'Software\\NovaOS\\MSITest', 'Name', '[ProductName] [ProductVersion]', 'RegComp')"
q "CREATE TABLE \`CreateFolder\` (\`Directory_\` CHAR(72) NOT NULL, \`Component_\` CHAR(72) NOT NULL PRIMARY KEY \`Directory_\`, \`Component_\`)"
q "INSERT INTO \`CreateFolder\` (\`Directory_\`, \`Component_\`) VALUES ('DocsDir', 'Docs')"
q "CREATE TABLE \`CustomAction\` (\`Action\` CHAR(72) NOT NULL, \`Type\` SHORT NOT NULL, \`Source\` CHAR(72), \`Target\` CHAR(255) PRIMARY KEY \`Action\`)"
q "INSERT INTO \`CustomAction\` (\`Action\`, \`Type\`, \`Source\`, \`Target\`) VALUES ('SetDocsDir', 51, 'DOCSNOTE', 'docs in [DocsDir]')"
q "INSERT INTO \`CustomAction\` (\`Action\`, \`Type\`, \`Source\`, \`Target\`) VALUES ('RunSomething', 34, 'INSTALLDIR', 'hello.exe')"
q "CREATE TABLE \`InstallExecuteSequence\` (\`Action\` CHAR(72) NOT NULL, \`Condition\` CHAR(255), \`Sequence\` SHORT PRIMARY KEY \`Action\`)"
for a in "CostInitialize 800" "FileCost 900" "CostFinalize 1000" "InstallValidate 1400" "InstallInitialize 1500" "ProcessComponents 1600" "CreateFolders 3700" "InstallFiles 4000" "WriteRegistryValues 5000" "RegisterProduct 6100" "InstallFinalize 6600"; do
  set -- $a; q "INSERT INTO \`InstallExecuteSequence\` (\`Action\`, \`Condition\`, \`Sequence\`) VALUES ('$1', '', $2)"
done
q "INSERT INTO \`InstallExecuteSequence\` (\`Action\`, \`Condition\`, \`Sequence\`) VALUES ('SetDocsDir', 'NOT Installed AND ProductVersion = \"1.2.3\"', 1001)"
q "INSERT INTO \`InstallExecuteSequence\` (\`Action\`, \`Condition\`, \`Sequence\`) VALUES ('RunSomething', 'NEVER', 6500)"
q "CREATE TABLE \`Shortcut\` (\`Shortcut\` CHAR(72) NOT NULL, \`Directory_\` CHAR(72) NOT NULL, \`Name\` CHAR(128) NOT NULL LOCALIZABLE, \`Component_\` CHAR(72) NOT NULL, \`Target\` CHAR(72) NOT NULL, \`Arguments\` CHAR(255), \`Description\` CHAR(255) LOCALIZABLE, \`Hotkey\` SHORT, \`Icon_\` CHAR(72), \`IconIndex\` SHORT, \`ShowCmd\` SHORT, \`WkDir\` CHAR(72) PRIMARY KEY \`Shortcut\`)"
q "INSERT INTO \`Shortcut\` (\`Shortcut\`, \`Directory_\`, \`Name\`, \`Component_\`, \`Target\`, \`Arguments\`, \`Description\`, \`Hotkey\`, \`Icon_\`, \`IconIndex\`, \`ShowCmd\`, \`WkDir\`) VALUES ('sc1', 'MenuDir', 'MSITES~1|MSI Test App', 'MainExe', '[INSTALLDIR]hello.exe', '', 'Says hello', 0, '', 0, 1, 'INSTALLDIR')"
msibuild test.msi -a test.cab test.cab
