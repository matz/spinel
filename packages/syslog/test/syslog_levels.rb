# Each level writes a formatted message and answers Syslog; so does log
# with a priority. A level the mask filters out is still a call that
# answers. (What reaches the system log is not read back here.) The mask
# lets only debug through: an emerg that reached the system log would be
# broadcast to every terminal (systemd-journald) each time the test runs.
require "syslog"

Syslog.open("spinel-syslog-levels", Syslog::LOG_PID, Syslog::LOG_USER) do |s|
  s.mask = Syslog::LOG_MASK(Syslog::LOG_DEBUG)
  p s.emerg("spinel syslog test: emerg %d", 1).equal?(Syslog)
  p s.alert("spinel syslog test: alert %s", "a").equal?(Syslog)
  p s.crit("spinel syslog test: crit").equal?(Syslog)
  p s.err("spinel syslog test: err %05.1f", 1.5).equal?(Syslog)
  p s.warning("spinel syslog test: warning %x", 255).equal?(Syslog)
  p s.notice("spinel syslog test: notice %s %s", "two", "words").equal?(Syslog)
  p s.info("HISTORY: PID=%d UID=%d %s", Process.pid, Process.uid, "ls -l").equal?(Syslog)
  p s.debug("spinel syslog test: debug").equal?(Syslog)
  p s.log(Syslog::LOG_NOTICE, "spinel syslog test: log %s", "plain").equal?(Syslog)
  p s.log(Syslog::LOG_INFO | Syslog::LOG_LOCAL0, "spinel syslog test: with a facility").equal?(Syslog)
  s.mask = Syslog::LOG_UPTO(Syslog::LOG_ERR)
  p s.debug("spinel syslog test: filtered by the mask").equal?(Syslog)
  p s.info("spinel syslog test: 100%% literal").equal?(Syslog)
end
p Syslog.opened?
