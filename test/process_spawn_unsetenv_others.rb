# The child inherits the parent's environment unless unsetenv_others is
# true. Clearing it keeps only non-nil entries from the environment Hash.
# /usr/bin/env avoids a shell adding its own variables to an empty env.
def spawn_environment(form, env, opts)
  r, w = IO.pipe
  pid = case form
        when "Process.spawn"
          Process.spawn(env, "/usr/bin/env", out: w, **opts)
        when "spawn"
          spawn(env, "/usr/bin/env", out: w, **opts)
        else
          Kernel.spawn(env, "/usr/bin/env", out: w, **opts)
        end
  w.close
  out = r.read
  r.close
  Process.wait(pid)
  out
end

def check_spawn_environment(out, cleared)
  lines = out.lines
  p lines.include?("SPAWN_UNSETENV_INHERITED=parent\n")
  p lines.include?("SPAWN_UNSETENV_ADDED=child\n")
  p lines.include?("SPAWN_UNSETENV_OVERRIDE=child\n")
  p lines.include?("SPAWN_UNSETENV_REMOVED=parent\n")
  p lines.include?("SPAWN_UNSETENV_EMPTY=\n")
  if cleared
    p lines.sort == ["SPAWN_UNSETENV_ADDED=child\n", "SPAWN_UNSETENV_EMPTY=\n",
                     "SPAWN_UNSETENV_OVERRIDE=child\n"]
  end
end

ENV["SPAWN_UNSETENV_INHERITED"] = "parent"
ENV["SPAWN_UNSETENV_OVERRIDE"] = "parent"
ENV["SPAWN_UNSETENV_REMOVED"] = "parent"
ENV.delete("SPAWN_UNSETENV_ADDED")
ENV.delete("SPAWN_UNSETENV_EMPTY")
changes = { "SPAWN_UNSETENV_ADDED" => "child", "SPAWN_UNSETENV_OVERRIDE" => "child",
            "SPAWN_UNSETENV_REMOVED" => nil, "SPAWN_UNSETENV_EMPTY" => "" }.freeze

["Process.spawn", "spawn", "Kernel.spawn"].each do |form|
  puts form
  check_spawn_environment(spawn_environment(form, changes, {}), false)
  check_spawn_environment(spawn_environment(form, changes, { unsetenv_others: false }), false)
  check_spawn_environment(spawn_environment(form, changes, { unsetenv_others: nil }), false)
  check_spawn_environment(spawn_environment(form, changes, { unsetenv_others: true }), true)
  p spawn_environment(form, {}, { unsetenv_others: true }).empty?
end

# No environment Hash still means an empty child environment when clearing.
r, w = IO.pipe
pid = Process.spawn("/usr/bin/env", out: w, unsetenv_others: true)
w.close
p r.read.empty?
r.close
Process.wait(pid)

[0, "", :clear, Object.new].each do |bad|
  begin
    pid = Process.spawn("/usr/bin/env", out: File::NULL, unsetenv_others: bad)
    Process.wait(pid)
    puts "accepted invalid option"
  rescue ArgumentError => e
    p e.class
  end
end

# The parent and the frozen Hash keep their original values.
p ENV["SPAWN_UNSETENV_INHERITED"], ENV["SPAWN_UNSETENV_OVERRIDE"], ENV["SPAWN_UNSETENV_REMOVED"]
p ENV["SPAWN_UNSETENV_ADDED"], ENV["SPAWN_UNSETENV_EMPTY"], changes["SPAWN_UNSETENV_REMOVED"]
