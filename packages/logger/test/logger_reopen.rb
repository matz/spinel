require "logger"
require "stringio"

first = StringIO.new
second = StringIO.new
logger = Logger.new(first)
logger.formatter = proc { |_severity, _time, _progname, message| "#{message}\n" }

logger.reopen(second)
logger.info("after reopen")
p first.string
p second.string
