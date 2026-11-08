# Clearing the child environment does not remove the parent's PATH from
# command lookup. An explicit child PATH still selects its own command.
require "tmpdir"

def path_environment(env, command)
  r, w = IO.pipe
  begin
    pid = Process.spawn(env, command, out: w, unsetenv_others: true)
    w.close
    out = r.read
    Process.wait(pid)
    out
  ensure
    w.close unless w.closed?
    r.close
  end
end

dir = Dir.mktmpdir("spinel-unsetenv")
command = File.join(dir, "unsetenv-command")
blocked = File.join(dir, "unsetenv-blocked")
script = File.join(dir, "unsetenv-script")
saved_path = ENV["PATH"]
begin
  File.symlink("/usr/bin/env", command)
  File.write(blocked, "not executable\n")
  File.chmod(0o600, blocked)
  File.write(script, "printf '%s' \"$UNSETENV_SCRIPT\"\n")
  File.chmod(0o700, script)
  ENV["PATH"] = "/no-such-spinel-unsetenv-directory:#{dir}"
  p path_environment({}, "unsetenv-command").empty?
  p path_environment({ "PATH" => nil }, "unsetenv-command").empty?
  env = { "PATH" => dir, "UNSETENV_PATH" => "child" }
  p path_environment(env, "unsetenv-command").lines.sort == ["PATH=#{dir}\n", "UNSETENV_PATH=child\n"]
  p path_environment({ "UNSETENV_SCRIPT" => "child" }, "unsetenv-script")
  ["unsetenv-blocked", "unsetenv-missing"].each do |name|
    begin
      path_environment({}, name)
    rescue SystemCallError => e
      p e.class
    end
  end
  Dir.chdir(dir) do
    ENV["PATH"] = ":/no-such-spinel-unsetenv-directory"
    p path_environment({}, "unsetenv-command").empty?
  end
ensure
  ENV["PATH"] = saved_path
  File.delete(command)
  File.delete(blocked)
  File.delete(script)
  Dir.rmdir(dir)
end
