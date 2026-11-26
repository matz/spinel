# A Logger shared by several threads writes each line whole: 4 threads by
# 500 lines give 2000 well-formed lines, through #add and #<<
require "logger"
require "stringio"

io = StringIO.new
logger = Logger.new(io)
logger.formatter = proc { |_sev, _time, _prog, msg| "#{msg}\n" }
threads = 4.times.map do |t|
  Thread.new do
    250.times { |i| logger.info("t#{t} #{i}") }
    250.times { |i| logger << "t#{t} #{i + 250}\n" }
  end
end
threads.each { |th| th.join(30) }
lines = io.string.lines
p lines.length
p lines.count { |l| l.match?(/\At\d \d+\n\z/) }
p lines.uniq.length
