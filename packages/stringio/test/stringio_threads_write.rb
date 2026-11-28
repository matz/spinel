# Four threads writing one StringIO with no Mutex: under CRuby's GVL each
# write is whole, and here each runs under the StringIO's own lock, so no
# write is lost to another's growth of the buffer. Without the lock two
# writers reallocated the buffer at once and the program aborted with a
# corrupted heap. Four workers, since at one the writes cannot overlap.
require "stringio"
ENV["SPINEL_WORKERS"] = "4"

def spawn_writers(io)
  (0...4).map do |n|
    Thread.new do
      i = 0
      while i < 1000
        case i % 3
        when 0 then io.write("t#{n} #{"x" * 300}\n")
        when 1 then io.puts("t#{n} #{"y" * 200}")
        else io << "t#{n} #{"z" * 100}\n"
        end
        i += 1
      end
    end
  end
end

[StringIO.new, StringIO.new(+"")].each do |io|
  spawn_writers(io).each(&:join)
  lines = io.string.lines
  p io.string.bytesize
  p lines.length
  p lines.count { |l| l.match?(/\At\d (x{300}|y{200}|z{100})\n\z/) }
end
