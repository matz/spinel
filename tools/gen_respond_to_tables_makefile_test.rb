#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Executes the Makefile's lib/spinel_rt_responds_generated.h recipe
# itself (extracted verbatim, generator invocation swapped for a command
# that fails the same way a real self-check failure does: no output,
# non-zero exit), in an isolated tmpdir so nothing here can touch a real
# repo file. Regression test for the fail-closed fix: before it, this
# recipe's last command (`mv`) decided the exit status, so a failing
# generator still reported success and replaced the previous good header
# with empty output.
require "tmpdir"
require "open3"

ROOT = File.expand_path("..", __dir__)
makefile = File.read(File.join(ROOT, "Makefile"))
rule = makefile[/^lib\/spinel_rt_responds_generated\.h:.*\n(?:\t.*\n)+/] or
  abort "could not find the lib/spinel_rt_responds_generated.h rule in the Makefile"

# Recipe lines only (drop the rule/prerequisites line), tab and Make's
# leading "@" (echo-suppression, meaningless outside Make) stripped --
# what's left is shell, not Make syntax.
body = rule.lines.drop(1).map { |l| l.sub(/\A\t@?/, "") }.join
abort "the recipe no longer invokes the generator the way this test expects" \
  unless body.include?("ruby tools/gen_respond_to_tables.rb")

Dir.mktmpdir do |dir|
  good_path = File.join(dir, "good.h")
  File.write(good_path, "GOOD CONTENT\n")
  script = body.gsub("ruby tools/gen_respond_to_tables.rb", "false")
                .gsub("$@", good_path)
                .gsub("$$", "$") # Make's $$ is a literal $ outside a Makefile recipe
  out, status = Open3.capture2e("bash", "-c", script)
  abort "the recipe reported success for a failing generator" if status.success?
  abort "the recipe ran into a shell error unrelated to the forced failure: #{out}" \
    unless out.strip.empty?
  abort "the recipe corrupted the existing header on a failing generator run" \
    unless File.read(good_path) == "GOOD CONTENT\n"
end

puts "gen_respond_to_tables_makefile_test: fail-closed recipe verified"
