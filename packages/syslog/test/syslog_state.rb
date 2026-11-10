# The state of Syslog: nothing before open, the ident, options, facility
# and mask after it, the raises of CRuby (open twice, log or close or set
# the mask before open, an ident that is no String), open! and reopen as
# a close and an open, and the block form, which closes afterwards, also
# when the block raises.
require "syslog"

p [Syslog.opened?, Syslog.ident, Syslog.options, Syslog.facility, Syslog.mask]
p Syslog.inspect
p Syslog.instance.equal?(Syslog)

begin
  Syslog.log(Syslog::LOG_INFO, "x")
rescue RuntimeError => e
  p e.message
end
begin
  Syslog.close
rescue RuntimeError => e
  p e.message
end
begin
  Syslog.mask = 3
rescue RuntimeError => e
  p e.message
end
begin
  Syslog.open(1)
rescue TypeError => e
  p e.message
end

r = Syslog.open("spinel-syslog-test")
p r.equal?(Syslog)
p Syslog.opened?
p Syslog.ident
p Syslog.options == (Syslog::LOG_PID | Syslog::LOG_CONS)
p Syslog.facility == Syslog::LOG_USER
p Syslog.mask == Syslog::LOG_UPTO(Syslog::LOG_DEBUG)
p Syslog.inspect == "<#Syslog: opened=true, ident=\"spinel-syslog-test\", options=#{Syslog.options}, facility=#{Syslog.facility}, mask=#{Syslog.mask}>"

begin
  Syslog.open("again")
rescue RuntimeError => e
  p e.message
end

Syslog.mask = Syslog::LOG_UPTO(Syslog::LOG_ERR)
p Syslog.mask == Syslog::LOG_UPTO(Syslog::LOG_ERR)

p Syslog.open!("spinel-syslog-reopened", Syslog::LOG_NDELAY, Syslog::LOG_LOCAL1).ident
p [Syslog.options == Syslog::LOG_NDELAY, Syslog.facility == Syslog::LOG_LOCAL1]
p Syslog.reopen("spinel-syslog-reopened-2").ident
p Syslog.close
p [Syslog.opened?, Syslog.ident, Syslog.options, Syslog.facility, Syslog.mask]

v = Syslog.open("spinel-syslog-block") do |s|
  p s.equal?(Syslog)
  p Syslog.opened?
  42
end
p [v.equal?(Syslog), Syslog.opened?]
begin
  Syslog.open("spinel-syslog-block-2") { raise "in block" }
rescue RuntimeError => e
  p [e.message, Syslog.opened?]
end

# nil options and facility take the defaults
Syslog.open("spinel-syslog-nil", nil, nil)
p Syslog.options == (Syslog::LOG_PID | Syslog::LOG_CONS)
p Syslog.facility == Syslog::LOG_USER
Syslog.close
