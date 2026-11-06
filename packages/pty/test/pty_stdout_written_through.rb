# STDOUT on a terminal is written through: what a write call wrote is on the
# terminal when the call returns, though STDOUT.sync answers false. A prompt
# with no newline is shown before the program waits for its answer, or sleeps.
#
# The program runs itself on a pseudo-terminal, once for each case below, and
# prints what the child had shown before it was sent any input: its prompt.
# A prompt held back in a buffer is not there to read, and the case prints ""
# after the wait.
require "pty"
require "rbconfig"

case ARGV[0]
when "write_gets"
  STDOUT.write("A> ")
  line = STDIN.gets
  STDOUT.write("got ", line)
  exit
when "print_sleep"
  print "B> "
  sleep 30
  exit
when "print_getc"
  $stdout.print "C> "
  c = $stdin.getc
  $stdout.print "got ", c, "\n"
  exit
when "append_readline"
  STDOUT << "D" << "> "
  line = STDIN.readline
  STDOUT << "got " << line
  exit
when "printf_read"
  printf("%s> ", "E")
  s = STDIN.read(3)
  printf("got %s\n", s)
  exit
when "putc_each_line"
  putc "F"
  putc ">"
  STDIN.each_line { |l| print "got ", l; break }
  exit
when "sync_off"
  # sync turned on and off again leaves a terminal written through
  $stdout.sync = true
  $stdout.sync = false
  p $stdout.sync
  STDOUT.write("G> ")
  line = STDIN.gets
  STDOUT.write("got ", line)
  exit
when "reopen_back"
  # STDOUT moved onto a pipe and back onto the terminal
  r, w = IO.pipe
  term = $stdout.dup
  $stdout.reopen(w)
  print "to the pipe"
  $stdout.reopen(term)
  w.close
  STDOUT.write("H> ")
  line = STDIN.gets
  STDOUT.write("got ", line, r.read, "\n")
  exit
end

# the same program again: the compiled one is its own file, CRuby's is the script
SELF = RUBY_ENGINE == "spinel" ? [$0] : [RbConfig.ruby, $0]

# What the child writes until `want` has arrived, or until it has written
# nothing for five seconds.
def shown(reader, want)
  out = ""
  until out.include?(want)
    break unless IO.select([reader], nil, nil, 5)
    out = out + reader.readpartial(1024)
  end
  out.delete("\r")
end

def drain(reader)
  out = ""
  begin
    loop { out = out + reader.readpartial(1024) }
  rescue Errno::EIO, EOFError
    nil
  end
  out.delete("\r")
end

[["write_gets", "A> "], ["print_getc", "C> "], ["append_readline", "D> "], ["printf_read", "E> "],
 ["putc_each_line", "F>"], ["sync_off", "G> "], ["reopen_back", "H> "]].each do |name, prompt|
  reader, writer, pid = PTY.spawn(*SELF, name)
  puts "#{name}: #{shown(reader, prompt).inspect} before input"
  writer.write("hello\n")
  puts "#{name}: #{drain(reader).inspect} after"
  Process.waitpid2(pid)
end

# no input here: the child is asleep with its prompt on the terminal
reader, _writer, pid = PTY.spawn(*SELF, "print_sleep")
puts "print_sleep: #{shown(reader, "B> ").inspect} while asleep"
Process.kill("KILL", pid)
Process.waitpid2(pid)
