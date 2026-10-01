# Logger -- a compact implementation of the stdlib Logger API: severities,
# level filtering, progname, a formatter proc (or the default
# "S, [time #pid] SEVERITY -- progname: message" line), blocks for lazy
# messages, and IO / file-name / nil destinations.
class Logger
  module Severity
    DEBUG = 0
    INFO = 1
    WARN = 2
    ERROR = 3
    FATAL = 4
    UNKNOWN = 5
  end
  include Severity

  SEV_LABEL = ["DEBUG", "INFO", "WARN", "ERROR", "FATAL", "ANY"]

  class Formatter
    attr_accessor :datetime_format

    def initialize
      @datetime_format = nil
    end

    def call(severity, time, progname, msg)
      t = @datetime_format ? time.strftime(@datetime_format) : time.strftime("%Y-%m-%dT%H:%M:%S.%6N")
      "#{severity[0]}, [#{t} ##{Process.pid}] #{severity.rjust(5)} -- #{progname}: #{msg2str(msg)}\n"
    end

    def msg2str(msg)
      if msg.is_a?(String)
        msg
      elsif msg.is_a?(Exception)
        "#{msg.message} (#{msg.class})"
      else
        msg.inspect
      end
    end
  end

  attr_accessor :progname, :formatter
  attr_reader :level

  def initialize(logdev = nil, shift_age = 0, shift_size = 1048576, level: DEBUG,
                 progname: nil, formatter: nil, datetime_format: nil)
    @logdev = nil
    @own = false
    @filename = nil
    case logdev
    when String
      @logdev = File.open(logdev.to_s, "a")
      @logdev.sync = true
      @own = true
      @filename = logdev
    when nil
    else
      @logdev = logdev
    end
    @progname = progname
    @formatter = formatter
    @default_formatter = Formatter.new
    @default_formatter.datetime_format = datetime_format
    @level = 0
    self.level = level
  end

  def level=(sev)
    @level = Logger.coerce_level(sev)
  end

  def self.coerce_level(sev)
    return sev if sev.is_a?(Integer)
    case sev.to_s.downcase
    when "debug" then DEBUG
    when "info" then INFO
    when "warn" then WARN
    when "error" then ERROR
    when "fatal" then FATAL
    when "unknown" then UNKNOWN
    else raise ArgumentError, "invalid log level: #{sev}"
    end
  end

  def datetime_format=(f)
    @default_formatter.datetime_format = f
  end

  def datetime_format
    @default_formatter.datetime_format
  end

  def debug?; @level <= DEBUG; end
  def info?; @level <= INFO; end
  def warn?; @level <= WARN; end
  def error?; @level <= ERROR; end
  def fatal?; @level <= FATAL; end

  def debug!; @level = DEBUG; end
  def info!; @level = INFO; end
  def warn!; @level = WARN; end
  def error!; @level = ERROR; end
  def fatal!; @level = FATAL; end

  def add(severity, message = nil, progname = nil, &block)
    severity = UNKNOWN if severity.nil?
    return true if @logdev.nil? || severity < @level
    progname = @progname if progname.nil?
    if message.nil?
      if block
        message = block.call
      else
        message = progname
        progname = @progname
      end
    end
    label = SEV_LABEL[severity] || "ANY"
    line = if @formatter
      @formatter.call(label, Time.now, progname, message)
    else
      @default_formatter.call(label, Time.now, progname, message)
    end
    @logdev.write(line)
    true
  end
  alias log add

  def debug(progname = nil, &block); add(DEBUG, nil, progname, &block); end
  def info(progname = nil, &block); add(INFO, nil, progname, &block); end
  def warn(progname = nil, &block); add(WARN, nil, progname, &block); end
  def error(progname = nil, &block); add(ERROR, nil, progname, &block); end
  def fatal(progname = nil, &block); add(FATAL, nil, progname, &block); end
  def unknown(progname = nil, &block); add(UNKNOWN, nil, progname, &block); end

  def <<(msg)
    @logdev.write(msg) if @logdev
  end

  # Without an argument a logger writing to a file it opened by name opens
  # that name again, as CRuby's does: the file may have been rotated away.
  def reopen(logdev = nil)
    logdev = @filename if logdev.nil?
    return self if @logdev.nil? || logdev.nil?

    if logdev.is_a?(String)
      new_logdev = File.open(logdev.to_s, "a")
      new_logdev.sync = true
      close
      @logdev = new_logdev
      @own = true
      @filename = logdev
    else
      close
      @logdev = logdev
      @own = false
      @filename = nil
    end
    self
  end

  def close
    @logdev.close if @own && @logdev
  end
end
