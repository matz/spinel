# A `return` in a block leaves the method the block is written in. When that
# method itself takes a block (so it is spliced into its caller), the return
# leaves the splice and the caller goes on; it used to end the program.
# A begin/ensure around such a return, whose value nothing reads, compiles.
class Recorder
  def synchronize = yield

  def capture
    synchronize do
      log = []
      yield log
      return log.size, log.join(",")
    end
  end

  def guarded
    synchronize do
      begin
        yield
        return :done
      ensure
        puts "ensure ran"
      end
    end
  end
end

n, s = Recorder.new.capture { |log| log << "a" << "b" }
p [n, s]
p Recorder.new.guarded { puts "in block" }
puts "after"
