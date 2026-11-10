# Spinel bundled `syslog` -- a carried-C spin package (Path B).
#
# CRuby's Syslog module over syslog(3): open, open! and reopen (with the
# block form, which closes afterwards), close, log and the eight levels,
# opened?, ident, options, facility, mask and mask=, LOG_MASK and LOG_UPTO,
# and the LOG_ constants, each the platform's value, in the modules a
# program includes for them (Syslog::Constants, and Level, Option, Facility
# and Macros under it). The C (sp_syslog.c, linked only when
# `require "syslog"` appears) is the four library calls and the constants'
# values; the state and the rules of CRuby (open twice raises, a log before
# open raises) are here.
#
# Not here: syslog/logger, the Logger adapter.
module Syslog
  module Native
    native_lib "syslog"
    native_obj "packages/syslog/sp_syslog.o"
    native_func :open,    [:string, :int, :int], :void, "sp_syslog_open"
    native_func :close,   [],                    :void, "sp_syslog_close"
    native_func :log,     [:int, :string],       :void, "sp_syslog_log"
    native_func :setmask, [:int],                :int,  "sp_syslog_setmask"
    native_func :const,   [:string],             :int,  "sp_syslog_const"
  end

  VERSION = "0.4.0"

  module Level
    LOG_EMERG = Native.const("LOG_EMERG")
    LOG_ALERT = Native.const("LOG_ALERT")
    LOG_CRIT = Native.const("LOG_CRIT")
    LOG_ERR = Native.const("LOG_ERR")
    LOG_WARNING = Native.const("LOG_WARNING")
    LOG_NOTICE = Native.const("LOG_NOTICE")
    LOG_INFO = Native.const("LOG_INFO")
    LOG_DEBUG = Native.const("LOG_DEBUG")
  end

  module Option
    LOG_PID = Native.const("LOG_PID")
    LOG_CONS = Native.const("LOG_CONS")
    LOG_ODELAY = Native.const("LOG_ODELAY")
    LOG_NDELAY = Native.const("LOG_NDELAY")
    LOG_NOWAIT = Native.const("LOG_NOWAIT")
    LOG_PERROR = Native.const("LOG_PERROR")
  end

  module Facility
    LOG_KERN = Native.const("LOG_KERN")
    LOG_USER = Native.const("LOG_USER")
    LOG_MAIL = Native.const("LOG_MAIL")
    LOG_DAEMON = Native.const("LOG_DAEMON")
    LOG_AUTH = Native.const("LOG_AUTH")
    LOG_SYSLOG = Native.const("LOG_SYSLOG")
    LOG_LPR = Native.const("LOG_LPR")
    LOG_NEWS = Native.const("LOG_NEWS")
    LOG_UUCP = Native.const("LOG_UUCP")
    LOG_CRON = Native.const("LOG_CRON")
    LOG_AUTHPRIV = Native.const("LOG_AUTHPRIV")
    LOG_FTP = Native.const("LOG_FTP")
    LOG_LOCAL0 = Native.const("LOG_LOCAL0")
    LOG_LOCAL1 = Native.const("LOG_LOCAL1")
    LOG_LOCAL2 = Native.const("LOG_LOCAL2")
    LOG_LOCAL3 = Native.const("LOG_LOCAL3")
    LOG_LOCAL4 = Native.const("LOG_LOCAL4")
    LOG_LOCAL5 = Native.const("LOG_LOCAL5")
    LOG_LOCAL6 = Native.const("LOG_LOCAL6")
    LOG_LOCAL7 = Native.const("LOG_LOCAL7")
  end

  # The macros of syslog.h: the mask bit of one priority, and the bits of
  # every priority up to one.
  module Macros
    def LOG_MASK(priority)
      1 << priority
    end

    def LOG_UPTO(priority)
      (1 << (priority + 1)) - 1
    end
  end

  module Constants
    include Level
    include Option
    include Facility
    include Macros
  end

  include Constants
  extend Macros

  @opened = false
  @ident = nil
  @options = nil
  @facility = nil
  @mask = nil

  def self.instance
    self
  end

  def self.opened?
    @opened
  end

  def self.ident
    @ident
  end

  def self.options
    @options
  end

  def self.facility
    @facility
  end

  def self.mask
    @mask
  end

  # Opens the log under ident (the program's name by default), with the
  # options (LOG_PID | LOG_CONS by default) and the facility (LOG_USER).
  # With a block, yields Syslog and closes afterwards. Answers Syslog.
  def self.open(ident = $0, options = nil, facility = nil)
    raise RuntimeError, "syslog already open" if @opened
    raise TypeError, "no implicit conversion of #{ident.class} into String" unless ident.is_a?(String)
    options = LOG_PID | LOG_CONS if options.nil?
    facility = LOG_USER if facility.nil?
    Native.open(ident, options, facility)
    @ident = ident
    @options = options
    @facility = facility
    @opened = true
    # setlogmask(0) asks for the mask and changes nothing
    @mask = Native.setmask(0)
    if block_given?
      begin
        yield self
      ensure
        close
      end
    end
    self
  end

  # A close when the log is open, then an open.
  def self.open!(ident = $0, options = nil, facility = nil, &block)
    close if @opened
    open(ident, options, facility, &block)
  end

  def self.reopen(ident = $0, options = nil, facility = nil, &block)
    open!(ident, options, facility, &block)
  end

  def self.close
    raise RuntimeError, "syslog not opened" unless @opened
    Native.close
    @opened = false
    @ident = nil
    @options = nil
    @facility = nil
    @mask = nil
    nil
  end

  # Writes the message format(fmt, *args) at the priority, which may carry
  # a facility (LOG_INFO | LOG_LOCAL0). Answers Syslog.
  def self.log(priority, fmt, *args)
    raise RuntimeError, "must open syslog before write" unless @opened
    Native.log(priority, format(fmt, *args))
    self
  end

  def self.mask=(mask)
    raise RuntimeError, "must open syslog before setting log mask" unless @opened
    Native.setmask(mask)
    @mask = mask
  end

  def self.emerg(fmt, *args)
    log(LOG_EMERG, fmt, *args)
  end

  def self.alert(fmt, *args)
    log(LOG_ALERT, fmt, *args)
  end

  def self.crit(fmt, *args)
    log(LOG_CRIT, fmt, *args)
  end

  def self.err(fmt, *args)
    log(LOG_ERR, fmt, *args)
  end

  def self.warning(fmt, *args)
    log(LOG_WARNING, fmt, *args)
  end

  def self.notice(fmt, *args)
    log(LOG_NOTICE, fmt, *args)
  end

  def self.info(fmt, *args)
    log(LOG_INFO, fmt, *args)
  end

  def self.debug(fmt, *args)
    log(LOG_DEBUG, fmt, *args)
  end

  def self.inspect
    if @opened
      "<#Syslog: opened=true, ident=#{@ident.inspect}, options=#{@options}, facility=#{@facility}, mask=#{@mask}>"
    else
      "<#Syslog: opened=false>"
    end
  end
end
