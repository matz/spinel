# An exec that fails raises its Errno: a program that is not there names
# the program, a directory of chdir: that is not there names the directory.
# The environment of the Hash is the command's, not this process's, so it is
# as it was; the pipe of in: is still open and readable, as under CRuby.
require "tmpdir"
r, w = IO.pipe
ENV["SP_EXEC_WAS"] = "1"
begin
  exec({ "SP_EXEC_GONE" => "yes", "SP_EXEC_WAS" => nil }, "sp_no_such_program_#{Process.pid}", "arg",
       in: r, chdir: Dir.tmpdir)
  puts "not reached"
rescue Errno::ENOENT => e
  puts "ENOENT names the program: #{e.message.include?("sp_no_such_program")}"
end
puts "environment kept: #{ENV["SP_EXEC_GONE"].inspect} #{ENV["SP_EXEC_WAS"].inspect}"
w.puts "still here"
w.close
puts "the pipe of in: is still readable: #{r.read.inspect}"

begin
  exec("sh", "-c", "echo not reached", chdir: "/sp_no_such_dir_#{Process.pid}")
rescue Errno::ENOENT => e
  puts "ENOENT names the directory: #{e.message.include?("sp_no_such_dir")}"
end
puts "end"
