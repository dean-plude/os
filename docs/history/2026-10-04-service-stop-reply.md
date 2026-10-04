## Services: a stopping service answers before its process exits

`svctest`'s `SERVICE_CONTROL_STOP` check failed now and then with
`ERROR_SERVICE_REQUEST_TIMEOUT` (1053), on main after the Steam service
change and on open pull requests.  A stop handler that reports
`SERVICE_STOPPED` sets the event `StartServiceCtrlDispatcher` waits on, so
the dispatcher could return and the service process end before the control
thread had written its answer; `ControlService` then read nothing.

- **`StartServiceCtrlDispatcher`** takes the control thread's lock before it
  returns, and the control thread holds that lock while it writes its
  answer, so the process stays until `ControlService` has its reply.
