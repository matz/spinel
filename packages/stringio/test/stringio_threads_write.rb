# Four threads writing one StringIO with no Mutex: under CRuby's GVL each
# write is whole, and here each runs under the StringIO's own lock, so no
# write is lost to another's growth of the buffer. Without the lock two
# writers reallocated the buffer at once and the program aborted with a
# corrupted heap. Four workers, since at one the writes cannot overlap.
# puts of an Integer or a Float writes a display String made for it, which
# nothing roots, and a collection may run while the write waits for another
# thread's: a write that waits writes a copy (SPINEL_GC_STRESS=2 wrote freed
# bytes). true, false and nil write literals, which take no String root.
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

def spawn_value_writers(io)
  (0...4).map do |n|
    Thread.new do
      i = 0
      while i < 1000
        k = i % 5
        v = k == 0 ? 1000000 + i : k == 1 ? (i * 0.5) + 0.25 : k == 2 ? true : k == 3 ? false : nil
        if k == 3 then io.print(v)
        elsif i % 3 == 0 then io.puts(v, i + 2000000)
        else io.puts(v)
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

[StringIO.new, StringIO.new(+"")].each do |io|
  spawn_value_writers(io).each(&:join)
  out = io.string
  p out.bytesize
  p out.count("\n")
  p out.scan(/\d+(?:\.\d+)?/).size
  p out.scan("true").size
  p out.scan("false").size
end
