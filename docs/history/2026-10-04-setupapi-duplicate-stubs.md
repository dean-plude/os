## setupapi: the build works again after two changes added the same functions

Two changes merged within minutes of each other each added
`CM_Get_Parent`, `CM_Get_Device_IDA` and `CM_Get_Device_IDW` to
`setupapi.dll`: one as stubs that find no device, the other as forwarders to
`cfgmgr32.dll`.  Together they defined each function twice, so the userland
no longer built.  The stubs are gone; the forwarders, which return what
`cfgmgr32.dll` reports, stay.
