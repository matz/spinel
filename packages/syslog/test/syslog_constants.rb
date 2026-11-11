# The constants of Syslog are the platform's: the eight priorities in
# order, the options as distinct bits, the facilities, LOG_MASK and
# LOG_UPTO as the macros compute them, and the modules a program includes
# for them (Syslog::Constants, with Level, Option, Facility and Macros).
require "syslog"

levels = [Syslog::LOG_EMERG, Syslog::LOG_ALERT, Syslog::LOG_CRIT, Syslog::LOG_ERR,
          Syslog::LOG_WARNING, Syslog::LOG_NOTICE, Syslog::LOG_INFO, Syslog::LOG_DEBUG]
p levels == (0..7).to_a

options = [Syslog::LOG_PID, Syslog::LOG_CONS, Syslog::LOG_ODELAY, Syslog::LOG_NDELAY,
           Syslog::LOG_NOWAIT, Syslog::LOG_PERROR]
p options.uniq.size
p options.all? { |o| o > 0 && (o & (o - 1)) == 0 }

facilities = [Syslog::LOG_KERN, Syslog::LOG_USER, Syslog::LOG_MAIL, Syslog::LOG_DAEMON,
              Syslog::LOG_AUTH, Syslog::LOG_SYSLOG, Syslog::LOG_LPR, Syslog::LOG_NEWS,
              Syslog::LOG_UUCP, Syslog::LOG_CRON, Syslog::LOG_AUTHPRIV, Syslog::LOG_FTP,
              Syslog::LOG_LOCAL0, Syslog::LOG_LOCAL1, Syslog::LOG_LOCAL2, Syslog::LOG_LOCAL3,
              Syslog::LOG_LOCAL4, Syslog::LOG_LOCAL5, Syslog::LOG_LOCAL6, Syslog::LOG_LOCAL7]
p facilities.uniq.size
p facilities.all? { |f| (f & 7) == 0 }
p Syslog::LOG_KERN
p Syslog::LOG_LOCAL7 > Syslog::LOG_LOCAL0

p Syslog::LOG_MASK(Syslog::LOG_ERR) == 1 << Syslog::LOG_ERR
p Syslog::LOG_UPTO(Syslog::LOG_ERR) == (1 << (Syslog::LOG_ERR + 1)) - 1
p Syslog.LOG_UPTO(Syslog::LOG_DEBUG)

p Syslog::Level::LOG_INFO == Syslog::LOG_INFO
p Syslog::Option::LOG_PID == Syslog::LOG_PID
p Syslog::Facility::LOG_USER == Syslog::LOG_USER
p Syslog::Constants::LOG_DEBUG == Syslog::LOG_DEBUG

class UsesTheConstants
  include Syslog::Constants
  def level = LOG_WARNING
  def mask = LOG_UPTO(LOG_WARNING)
end
u = UsesTheConstants.new
p u.level == Syslog::LOG_WARNING
p u.mask == Syslog::LOG_UPTO(Syslog::LOG_WARNING)
p Syslog::VERSION.is_a?(String)
