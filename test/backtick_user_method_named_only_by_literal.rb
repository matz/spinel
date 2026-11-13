# A backtick literal is a call of the method named `, so a program whose
# only use of that name is the literal still keeps its own def: unnamed
# methods are left out of the analysis (#11708), and a literal names `.

class Shell
  def `(cmd) = "Shell ran #{cmd}"
  def run = `ls -l`
end

module Fake
  def `(cmd) = cmd.upcase
end

class UsesFake
  include Fake
  def run(n) = %x(build #{n})
end

class Recorder
  def self.`(cmd) = cmd.length
  def self.run = `date`
end

puts Shell.new.run
puts UsesFake.new.run(2)
puts Recorder.run
