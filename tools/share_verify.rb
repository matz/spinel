#!/usr/bin/env ruby
# Compile the sharing corpus with both compiler shadows. Ratchet exact
# diagnostics, not whole files: another conflict in a known file must fail.
require "open3"
require "tmpdir"
require "set"
require "optparse"

root = File.expand_path("..", __dir__)
Dir.chdir(root)
options = { ratchet: "test/share/verify-conflicts.txt", extra: [], verbose: false }
OptionParser.new do |o|
  o.banner = "Usage: ruby tools/share_verify.rb [-v] [--extra DIR] [--ratchet FILE] [FILE.rb ...]"
  o.on("-v", "Print classified diagnostics too") { options[:verbose] = true }
  o.on("--extra DIR", "Include a review directory's *.rb (repeatable)") { |d| options[:extra] << d }
  o.on("--ratchet FILE") { |f| options[:ratchet] = f }
end.parse!
focused = !ARGV.empty?
files = focused ? ARGV : Dir.glob(["test/share/*.rb", "test/share/verify/**/*.rb", "test/share_strings_*.rb"])
files += options[:extra].flat_map { |d| Dir.glob(File.join(d, "*.rb")) }
files = files.uniq.sort
abort "share-verify: no programs" if files.empty?
spinel = File.expand_path(ENV.fetch("SPINEL", "bin/spinel"))
abort "share-verify: build bin/spinel first" unless File.executable?(spinel)
jobs = Integer(ENV.fetch("SHARE_VERIFY_JOBS", "2"))
abort "share-verify: SHARE_VERIFY_JOBS must be positive" unless jobs.positive?
known = File.readlines(options[:ratchet], chomp: true).reject { |s| s.empty? || s.start_with?("#") }
abort "share-verify: duplicate ratchet entries" unless known.uniq == known
known = known.to_set
queue = Queue.new
files.each { |file| queue << file }
results = Queue.new
workers = [jobs, files.size].min.times.map do
  Thread.new do
    loop do
      file = queue.pop(true)
      Dir.mktmpdir("spinel-share-verify-") do |tmp|
        env = { "TMPDIR" => tmp, "SPINEL_GC_STRESS" => "0" }
        checked = File.join(tmp, "checked.c")
        plain = File.join(tmp, "plain.c")
        args = ["-c", "--no-line-map", file, "-o"]
        _, err, status = Open3.capture3(env, spinel, "--repr-check", "--plan-check", *args, checked)
        _, off_err, off_status = Open3.capture3(env, spinel, *args, plain)
        diagnostics = err.lines.grep(/\A(?:repr|plan)-check:/).map(&:chomp)
        conflicts = diagnostics.grep(/\A(?:repr|plan)-check: [^:]*conflict:|\Aplan-check: (?:refuse-wrong|iter-row-error):/)
        failures = []
        failures << "compile failed (checked #{status.exitstatus}, plain #{off_status.exitstatus})" unless status.success? && off_status.success?
        if status.success? && off_status.success? && File.binread(checked) != File.binread(plain)
          failures << "generated C differs with the flags"
        end
        failures << "inference did not converge" if [err, off_err].any? { |s| s.include?("did not converge") }
        unless file.start_with?("test/share/verify/conflicts/")
          expected = "#{file}.expected"
          executable = File.join(tmp, "program")
          if !File.file?(expected)
            failures << "missing .expected"
          elsif failures.empty?
            _, build_err, build_status = Open3.capture3(env, spinel, "--repr-check", "--plan-check", "--jobs=1", file, "-o", executable)
            if build_status.success?
              out, run_status = Open3.capture2e(env, executable)
              failures << "program failed (#{run_status.exitstatus})" unless run_status.success?
              failures << "output differs from .expected" unless out.b == File.binread(expected)
            else
              failures << "executable build failed (#{build_status.exitstatus})"
              err += build_err
            end
          end
        end
        results << [file, conflicts, diagnostics, failures, err]
      end
    rescue ThreadError
      break
    end
  end
end
workers.each(&:value)
observed = Set.new
bad = 0
gaps = 0
rows = files.size.times.map { results.pop }.sort_by(&:first)
rows.each do |file, conflicts, diagnostics, failures, err|
  conflicts.each do |line|
    # A malformed builtin table is program-independent; keep its exact
    # diagnostic once, without granting a wildcard to per-node conflicts.
    key = line.start_with?("plan-check: iter-row-error:") ? "*" : file
    observed << "#{key}\t#{line}"
  end
  gaps += diagnostics.count { |s| s.match?(/channel-(?:unobserved|gap):/) }
  puts diagnostics.map { |s| "#{file}\t#{s}" } if options[:verbose]
  failures.each { |s| warn "share-verify: #{file}: #{s}"; bad += 1 }
  warn err unless failures.empty?
end
added = observed - known
STDOUT.flush
# A focused invocation checks only its selected files for stale entries.
selected = files.to_set
stale = known.select { |s| !focused || s.start_with?("*\t") || selected.include?(s.split("\t", 2).first) }.to_set - observed
added.sort.each { |s| warn "NEW\t#{s}" }
stale.sort.each { |s| warn "STALE (remove fixed entry)\t#{s}" }
puts "share-verify: #{files.size} programs, #{observed.size} known/conflicting diagnostics, #{added.size} new, #{stale.size} stale, #{bad} failures, #{gaps} channel gaps"
exit(added.empty? && bad.zero? ? 0 : 1)
