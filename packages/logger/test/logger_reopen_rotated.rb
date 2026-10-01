# reopen without an argument opens the logger's file name again, so a log
# rotated away by rename goes on in a new file under the old name; given a
# name it switches to that file, and given an IO it stops owning a file.
require "logger"
require "stringio"
require "tmpdir"

dir = Dir.tmpdir
a = File.join(dir, "sp_logger_reopen_a_#{Process.pid}.log")
b = File.join(dir, "sp_logger_reopen_b_#{Process.pid}.log")
rot = a + ".1"
logger = Logger.new(a)
logger.formatter = proc { |_s, _t, _p, m| "#{m}\n" }
logger.info("one")
File.rename(a, rot)
logger.info("two")
p logger.reopen.equal?(logger)
logger.info("three")
logger.reopen(b)
logger.info("four")
io = StringIO.new
logger.reopen(io)
logger.info("five")
logger.reopen
logger.info("six")
logger.close
# CRuby's logger starts a file it creates with a header line; skip it
body = proc { |f| File.readlines(f).reject { |l| l.start_with?("# Logfile") }.join }
p body.call(rot)
p body.call(a)
p body.call(b)
p io.string
[a, b, rot].each { |f| File.delete(f) if File.exist?(f) }
