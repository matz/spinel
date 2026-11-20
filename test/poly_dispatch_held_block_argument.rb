# frozen_string_literal: true
# spinel: gc-stress
# spinel: gc-minor

class Accept
  def run(arg, &blk) = [arg, blk.call]
end

class AlsoAccept
  def run(arg, &blk) = [arg, blk.call]
end

def collect
  $argument = nil
  100.times { [1, 2, 3] }
  GC.start
  proc { :block }
end

def exercise(receiver)
  $argument = ["held"]
  p receiver.run($argument, &collect)
end

exercise(ARGV.empty? ? Accept.new : AlsoAccept.new)
