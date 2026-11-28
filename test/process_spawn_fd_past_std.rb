# spawn and exec with a redirection of a descriptor past 2: the child gets
# the IO, the number, the file or :close on that descriptor, beside the
# standard streams and the other options.
r, w = IO.pipe
pid = spawn("sh", "-c", "echo hi >&3; echo out", 3 => w, out: File::NULL)
w.close
p r.read
Process.wait(pid)
p $?.exitstatus, $?.pid == pid
r.close

# two descriptors, one to read and one to write
r2, w2 = IO.pipe
w2.write("abc\n")
w2.close
r, w = IO.pipe
pid = spawn("sh", "-c", "read x <&4; echo got-$x >&5", 4 => r2, 5 => w)
w.close
p r.read
Process.wait(pid)
r.close
r2.close

# a number names the descriptor of this process, and an IO key its fileno
r, w = IO.pipe
pid = spawn("sh", "-c", "echo num >&7", 7 => w.fileno)
w.close
p r.read
Process.wait(pid)
r.close
r, w = IO.pipe
pid = spawn("sh", "-c", "echo key >&#{w.fileno}", w => w)
w.close
p r.read
Process.wait(pid)
r.close

# a source is a descriptor of this process as it is before the standard
# streams move: 3 is the pipe, and out is the null device
r, w = IO.pipe
save = $stdout.dup
$stdout.reopen(w)
pid = spawn("sh", "-c", "echo three >&3; echo one", 3 => $stdout, out: File::NULL)
$stdout.reopen(save)
save.close
w.close
p r.read
Process.wait(pid)
r.close

# a file by its path and mode, with chdir and an environment
require "tmpdir"
Dir.mktmpdir do |d|
  path = File.join(d, "three.txt")
  pid = spawn({ "SPAWN_FD_VAR" => "v" }, "sh", "-c", "echo $SPAWN_FD_VAR $(pwd) >&3", 3 => [path, "w"], chdir: d)
  Process.wait(pid)
  p File.read(path) == "v #{File.realpath(d)}\n"
  pid = spawn("sh", "-c", "echo again >&3", 3 => [path, File::WRONLY | File::APPEND, 0o644])
  Process.wait(pid)
  p File.read(path).lines.last
  pid = spawn("sh", "-c", "cat <&3 >&4", 3 => path, 4 => [File.join(d, "copy.txt"), "w"])
  Process.wait(pid)
  p File.read(File.join(d, "copy.txt")).lines.size
  # a group shares one open file, and [:child, n] is a descriptor of the child
  pid = spawn("sh", "-c", "echo a >&5; echo b >&6; echo c >&7", [5, 6] => [File.join(d, "group.txt"), "w"], 7 => [:child, 5])
  Process.wait(pid)
  p File.read(File.join(d, "group.txt"))
  File.open(File.join(d, "mixed.txt"), "w") do |f|
    pid = spawn("sh", "-c", "echo o; echo x >&8", [:out, 8] => f)
    Process.wait(pid)
  end
  p File.read(File.join(d, "mixed.txt"))
  # a path is one of the directory of the call, not of chdir:
  Dir.mkdir(File.join(d, "sub"))
  Dir.chdir(d) do
    File.write("rel.txt", "of the call\n")
    File.write(File.join("sub", "rel.txt"), "of chdir\n")
    pid = spawn("sh", "-c", "cat <&3 >&4", 3 => "rel.txt", 4 => ["rel_out.txt", "w"], chdir: "sub")
    Process.wait(pid)
    p File.read("rel_out.txt"), File.exist?(File.join("sub", "rel_out.txt"))
  end
  # a group with a standard stream, to a file by its path and mode
  pid = spawn("sh", "-c", "echo o; echo t >&3", [1, 3] => [File.join(d, "log.txt"), "w"])
  Process.wait(pid)
  p File.read(File.join(d, "log.txt"))
  # a path with no mode is opened to read, for a group with a member past 2
  # and for `[path]` on a standard stream: the file stays as it is
  File.write(File.join(d, "keep.txt"), "keep\n")
  pid = spawn("sh", "-c", "cat <&3", [1, 3] => File.join(d, "keep.txt"), err: File::NULL)
  Process.wait(pid)
  pid = spawn("true", out: [File.join(d, "keep.txt")])
  Process.wait(pid)
  p File.read(File.join(d, "keep.txt"))
  # an IO in a group names its descriptor
  File.open(File.join(d, "io.txt"), "w") do |f|
    pid = spawn("sh", "-c", "echo o; echo t >&3", [$stdout, 3] => f)
    Process.wait(pid)
  end
  p File.read(File.join(d, "io.txt"))
  # [:child, n] is the descriptor as the options make it, whatever their order
  File.open(File.join(d, "a.txt"), "w") do |f|
    pid = spawn("sh", "-c", "echo x >&3", 3 => [:child, f.fileno], f.fileno => [File.join(d, "b.txt"), "w"])
    Process.wait(pid)
  end
  p File.read(File.join(d, "a.txt")), File.read(File.join(d, "b.txt"))
  # the files a call that fails opened are closed again
  before = Dir.children("/dev/fd").size
  3.times do
    begin
      spawn("true", 3 => File.join(d, "a.txt"), 4 => File.join(d, "no", "such"))
    rescue SystemCallError
    end
  end
  p Dir.children("/dev/fd").size == before
  # a file that does not open raises, with the path, and leaves no child
  begin
    spawn("true", 3 => File.join(d, "no", "such"))
  rescue SystemCallError => e
    p e.class, e.message.end_with?(File.join("no", "such"))
  end
  p $?.exitstatus
end

# the file a standard stream goes to is opened on the lowest free
# descriptor, which the call may redirect too
lowest = IO.pipe.then { |a, b| n = a.fileno; a.close; b.close; n }
r, w = IO.pipe
Dir.mktmpdir do |d|
  path = File.join(d, "in.txt")
  File.write(path, "from file\n")
  pid = spawn("sh", "-c", "cat <&#{lowest}; echo tail >&2", lowest => path, out: w, err: File::NULL)
  w.close
  p r.read
  Process.wait(pid)
  p $?.exitstatus
end
r.close

# :close: the child has no descriptor 3
r, w = IO.pipe
pid = spawn("sh", "-c", "echo x >&#{w.fileno}", w.fileno => :close, err: File::NULL)
Process.wait(pid)
p $?.success?
w.close
p r.read
r.close

# the parent keeps its descriptor
r, w = IO.pipe
Process.wait(spawn("sh", "-c", "echo one >&3", 3 => w))
w.write("two\n")
w.close
p r.read
r.close

# a program that cannot run raises, with no child left behind
r, w = IO.pipe
begin
  spawn("no_such_program_for_spawn_fd", 3 => w)
rescue SystemCallError => e
  p e.class
end
w.close
p r.read
r.close
p((Process.wait rescue $!.class))

# a thread other than the main one
t = Thread.new do
  r, w = IO.pipe
  pid = spawn("sh", "-c", "echo thread >&3", 3 => w)
  w.close
  s = r.read
  Process.wait(pid)
  r.close
  s
end
p t.value

# each member of a group that is closed is closed
r, w = IO.pipe
r3, w3 = IO.pipe
pid = spawn("sh", "-c", "echo a >&#{w.fileno}; echo b >&#{w3.fileno}", [w.fileno, w3.fileno] => :close, err: File::NULL)
Process.wait(pid)
w.close
w3.close
p r.read, r3.read
r.close
r3.close

# a source that is not open raises
closed = IO.pipe.then { |a, b| n = a.fileno; a.close; b.close; n }
begin
  spawn("true", 3 => closed)
rescue SystemCallError => e
  p e.class
end

# a number that is no descriptor: past a C int, or negative
[{ 3 => 0x100000001 }, { 0x100000001 => 1 }, { -1 => 1 }, { 3 => [:child, 0x100000001] },
 { [1, 3] => 0x100000001 }].each do |opts|
  begin
    spawn("true", opts)
  rescue RangeError, ArgumentError => e
    puts "#{e.class}: #{e.message}"
  end
end

# a value that is no redirection, a descriptor of the child that no option
# makes, and a loop of them
[{ 3 => 1.5 }, { 3 => [:child, 9] }, { 3 => [:child, 4], 4 => [:child, 3] }].each do |opts|
  begin
    spawn("true", opts)
  rescue ArgumentError => e
    puts e.message
  end
end

# exec makes them too: the command writes to this program's output
$stdout.flush
exec("sh", "-c", "echo exec >&3", 3 => $stdout)
