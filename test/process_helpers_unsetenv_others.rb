# system with options and the reading IO.popen block both use spawn's
# environment setup. Check the complete child environment when clearing it.
require "tmpdir"

def helper_environment(form, env, opts)
  if form == "IO.popen"
    return IO.popen([env, "/usr/bin/env"], **opts) { |io| io.read }
  end
  # A file, not a pipe: an inherited environment can exceed a pipe's
  # capacity, and system waits for the child before anything reads it.
  out = ""
  Dir.mktmpdir do |dir|
    path = File.join(dir, "env.txt")
    system(env, "/usr/bin/env", out: path, **opts)
    out = File.read(path)
  end
  out
end

ENV["HELPER_UNSETENV_INHERITED"] = "parent"
ENV["HELPER_UNSETENV_OVERRIDE"] = "parent"
ENV["HELPER_UNSETENV_REMOVED"] = "parent"
ENV.delete("HELPER_UNSETENV_ADDED")
changes = { "HELPER_UNSETENV_ADDED" => "child", "HELPER_UNSETENV_OVERRIDE" => "child",
            "HELPER_UNSETENV_REMOVED" => nil }.freeze

["system", "IO.popen"].each do |form|
  puts form
  [{}, { unsetenv_others: false }, { unsetenv_others: nil }, { unsetenv_others: true }].each do |opts|
    lines = helper_environment(form, changes, opts).lines
    p lines.include?("HELPER_UNSETENV_INHERITED=parent\n")
    p lines.include?("HELPER_UNSETENV_ADDED=child\n")
    p lines.include?("HELPER_UNSETENV_OVERRIDE=child\n")
    p lines.include?("HELPER_UNSETENV_REMOVED=parent\n")
    if opts[:unsetenv_others]
      p lines.sort == ["HELPER_UNSETENV_ADDED=child\n", "HELPER_UNSETENV_OVERRIDE=child\n"]
    end
  end
  p helper_environment(form, {}, { unsetenv_others: true }).empty?
end

p ENV["HELPER_UNSETENV_INHERITED"], ENV["HELPER_UNSETENV_OVERRIDE"], ENV["HELPER_UNSETENV_REMOVED"]
p ENV["HELPER_UNSETENV_ADDED"]
