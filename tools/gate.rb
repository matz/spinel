require "etc"
require "fileutils"
require "open3"
require "tmpdir"

module Gate
  IMAGE = "spinel-gate"
  CONTAINER = <<~'SH'
    tar -x -C /src && cd /src
    cache="vendor/prism vendor/rbs build/rubyspec build/optcarrot build/rubyboy"
    for d in $cache; do [ -d /cache/$d ] && mkdir -p $(dirname $d) && cp -a /cache/$d $d; done
    chown -R gate:gate /src
    runuser -u gate -- env HOME=/home/gate PATH="$PATH" bash -c 'cd /src && git init -q && git add -A &&
      git -c user.email=gate@local -c user.name=gate commit -qm gate &&
      for i in 1 2 3 4 5; do make deps >/dev/null 2>&1 && break; sleep 5; done; make -j$(nproc) gate 2>&1'
    rc=$?
    for d in $cache; do [ -d $d ] && [ ! -d /cache/$d ] && mkdir -p /cache/$(dirname $d) && cp -a $d /cache/$d; done
    echo "@@platform linux-$(uname -m) gcc-$(gcc -dumpfullversion)"
    exit $rc
  SH

  module_function

  def run(*cmd, env: {})
    out, st = Open3.capture2(env, *cmd, err: File::NULL, binmode: true)
    out if st.success?
  rescue SystemCallError
    nil
  end

  def git(*args, env: {}) = run("git", *args, env: env)&.strip

  # A file's bytes, whatever the locale: under a C locale git's output reads
  # as US-ASCII, and the scans below raised on a UTF-8 source.
  def show(spec) = run("git", "show", spec).to_s.b

  def master
    [ENV["GATE_MASTER"], "upstream/master", "origin/master"].compact
      .lazy.filter_map { |ref| git("rev-parse", "-q", "--verify", "#{ref}^{commit}") }.first
  end

  def merged_tree(base, commit) = git("merge-tree", "--write-tree", base, commit)

  def stamp_path = File.join(git("rev-parse", "--git-dir"), "gate-stamp")

  def start_path = File.join(git("rev-parse", "--git-dir"), "gate-start")

  def write_stamp(tree, base, platform, tests)
    File.write(stamp_path, "tree=#{tree}\nmaster=#{base}\nplatform=#{platform}\ntests=#{tests}\n" \
                           "date=#{Time.now.utc.strftime("%FT%TZ")}\n")
    puts "gate: stamp for tree #{tree[0, 12]} on master #{base[0, 12]}; " \
         "git commit --amend --no-edit adds the Gate: trailer"
  end

  def compiler
    cc = (ENV["CC"] || "cc").split.reject { |w| %w[ccache sccache].include?(File.basename(w)) }.first || "cc"
    kind = run(cc, "--version").to_s.include?("clang") ? "clang" : "gcc"
    "#{kind}-#{(run(cc, "-dumpfullversion") || run(cc, "-dumpversion")).to_s.strip}"
  end

  def worktree_tree
    Dir.mktmpdir do |dir|
      index = { "GIT_INDEX_FILE" => File.join(dir, "index") }
      FileUtils.cp(File.join(git("rev-parse", "--git-dir"), "index"), index["GIT_INDEX_FILE"])
      git("add", "-A", env: index) && git("write-tree", env: index)
    end
  end

  def start
    master and File.write(start_path, worktree_tree.to_s)
  end

  def stamp
    base = master or return
    tree = worktree_tree or return
    started = File.exist?(start_path) && File.read(start_path)
    File.delete(start_path) if started
    return warn("gate: the tree is not the one the gate started on; no stamp") unless started == tree

    heads = Dir["build/test-results/*.ok"].map { |f| File.open(f, &:gets).to_s }
    tests = "#{heads.count { |l| l.start_with?("PASS") }}/#{heads.count { |l| l.start_with?("FAIL", "ERR") }}"
    u = Etc.uname
    write_stamp(tree, base, "#{u[:sysname].downcase}-#{u[:machine]} #{compiler}", tests)
  end

  def trailer(msg)
    File.write(msg, File.readlines(msg).reject { |l| l.start_with?("Gate: ") }.join)
    return unless File.exist?(stamp_path)

    s = File.readlines(stamp_path, chomp: true).to_h { |l| l.split("=", 2) }
    parent = git("rev-parse", "-q", "--verify", "HEAD") or return
    probe = git("commit-tree", git("write-tree"), "-p", parent, "-m", "gate-probe") or return
    return unless merged_tree(s["master"], probe) == s["tree"]

    system("git", "interpret-trailers", "--in-place", "--if-exists", "replace", "--trailer",
           "Gate: green tree #{s["tree"][0, 12]} master #{s["master"][0, 12]} (#{s["platform"]}) tests #{s["tests"]}", msg)
  end

  def verify(commit)
    short = git("rev-parse", "--short", commit) or return warn("gate: no commit #{commit}") || 2
    line = git("log", "-1", "--format=%(trailers:key=Gate,valueonly)", commit).lines.first.to_s.strip
    return puts("NO GATE TRAILER on #{short}") || 2 if line.empty?

    tested = line[/tree (\h{12,})/, 1]
    base = git("rev-parse", "-q", "--verify", "#{line[/master (\h+)/, 1]}^{commit}")
    return puts("UNKNOWN master in #{line.inspect}; fetch it") || 3 unless base

    tree = merged_tree(base, commit).to_s
    return puts("MISMATCH #{short}: merged tree #{tree[0, 12]}, tested #{tested}") || 1 unless tested && tree.start_with?(tested)

    puts "OK #{short}: merged with #{base[0, 12]} it is tree #{tree[0, 12]}, as tested (#{line})"
    0
  end

  def functions(src)
    out = {}
    name = start = nil
    src.each_line.with_index(1) do |line, i|
      if start.nil? && line !~ /\A(?:if|for|while|switch|return|else)\b/ &&
         (m = line.match(/\A(?:static\s+)?(?:inline\s+)?[\w\s*]+?\b(\w+)\s*\([^;]*\)\s*\{\s*\z/))
        name = m[1]
        start = i
      elsif start && line.start_with?("}")
        out[name] = [out[name].to_i, i - start + 1].max
        start = nil
      end
    end
    out
  end

  # The CRuby that judges a new test's .expected: tools/gate-ruby's pick
  # ($GATE_RUBY, else a Ruby 4.0 or later on PATH), or nil after a warning.
  def reference_ruby
    ruby = run("sh", File.join(__dir__, "gate-ruby")).to_s.strip
    return ruby unless ruby.empty?

    warn "gate: no Ruby 4.0 or later (set GATE_RUBY); .expected not checked against CRuby"
  end

  # `# spinel: not-cruby` in a test's first lines: its .expected is spinel's
  # own answer and legitimately differs from CRuby's.
  def not_cruby?(src) = src.lines.first(5).any? { |l| l.start_with?("# spinel: not-cruby") }

  def cruby(ruby, t, args)
    stdin = File.exist?("#{t}.stdin") ? "#{t}.stdin" : File::NULL
    IO.popen([ruby, "--enable-frozen-string-literal", "--external-encoding=UTF-8", t, *args], in: stdin, err: File::NULL) do |io|
      reader = Thread.new { io.read }
      next reader.value if reader.join(20)

      Process.kill(:KILL, io.pid)
      reader.join
      nil
    end
  end

  # The function-size rule (#7033), whole: emit_call_body only shrinks, and a
  # function past FUNCTION_LIMIT lines neither grows nor is added. Returns
  # the complaint for function fn, `was` lines at HEAD (nil when new) and n
  # lines staged, or nil; check refuses the commit on it.
  FUNCTION_LIMIT = 1000

  def function_size_error(fn, was, n)
    if fn == "emit_call_body" && was && n > was
      "emit_call_body grew #{was} -> #{n}; it only shrinks (#7033)"
    elsif was && was > FUNCTION_LIMIT && n > was
      "#{fn} grew #{was} -> #{n}; add the arm through a helper or its receiver file"
    elsif was.nil? && n > FUNCTION_LIMIT
      "new function #{fn} is #{n} lines"
    end
  end

  # The change check judges: the staged one (the index against HEAD, the
  # pre-commit hook's), or a commit range's (head against its merge base
  # with base, CI's view of a pull request). `before` and `after` prefix a
  # path to name its blob, `diff` selects the change for git diff, and
  # `tree` is what the working tree must match for CRuby to run a test there.
  Change = Struct.new(:before, :after, :diff, :tree)

  def staged = Change.new("HEAD:", ":", ["--cached"], [])

  def range(base, head)
    b = git("rev-parse", "-q", "--verify", "#{base}^{commit}") or return
    h = git("rev-parse", "-q", "--verify", "#{head}^{commit}") or return
    mb = git("merge-base", b, h) or return
    Change.new("#{mb}:", "#{h}:", [mb, h], [h])
  end

  # The test harness (RUN_ONE_TEST) reads test/foo.rb.expected and, without
  # one, runs CRuby for the expected output; a test/foo.expected is never read.
  def no_expected_error(t, changed)
    alt = t.sub(/\.rb\z/, ".expected")
    hint = changed.include?(alt) || File.exist?(alt) ? "; rename #{alt}: the harness reads only #{t}.expected" : ""
    "#{t}: no #{t}.expected#{hint}"
  end

  def check(change = staged)
    errors = []
    changed = git("diff", *change.diff, "--name-only", "--diff-filter=ACMR").to_s.split("\n")
    changed.grep(%r{\Asrc/.*\.c\z}).each do |f|
      before = functions(show("#{change.before}#{f}"))
      functions(show("#{change.after}#{f}")).each do |fn, n|
        e = function_size_error(fn, before[fn], n) and errors << "#{f}: #{e}"
      end
    end
    added = git("diff", *change.diff, "--name-only", "--diff-filter=A", "--", "test/*.rb").to_s.split("\n")
      .reject { |t| t.count("/") > 1 }
    ruby = reference_ruby unless added.empty?
    added.each do |t|
      src = show("#{change.after}#{t}")
      errors << "#{t}: use Dir.tmpdir, not a fixed /tmp path" if src.match?(%r{["']/tmp/})
      if !src.include?("# spinel: int64") && src.scan(/(?<![\w.])\d[\d_]{9,}/).any? { |n| n.delete("_").to_i >= 2**31 }
        warn "gate: #{t} has literals past 2^31 but no `# spinel: int64` marker"
      end
      next errors << no_expected_error(t, changed) unless changed.include?("#{t}.expected")
      next if !ruby || not_cruby?(src)

      unless git("diff", "--quiet", *change.tree, "--", t, "#{t}.args", "#{t}.stdin")
        next warn("gate: #{t} differs in the working tree; .expected not checked")
      end

      out = cruby(ruby, t, File.exist?("#{t}.args") ? File.binread("#{t}.args").split : [])
      next warn("gate: #{t} ran over 20s under CRuby; .expected not checked") unless out

      errors << "#{t}: .expected differs from `#{ruby} --enable-frozen-string-literal #{t}`" if out.b != show("#{change.after}#{t}.expected")
    end
    errors.each { |e| warn "gate: #{e}" }
    errors.empty? ? 0 : 1
  end

  def check_range(base, head)
    change = range(base, head) or return warn("gate: no merge base for #{base} and #{head}; fetch them") || 2
    check(change)
  end

  def linux
    Dir.chdir(git("rev-parse", "--show-toplevel") || abort("gate: not in a git checkout"))
    base = master or abort "gate: no upstream/master or origin/master; set GATE_MASTER"
    tree = merged_tree(base, "HEAD") or abort "gate: HEAD does not merge cleanly with #{base[0, 12]}; rebase first"
    run("docker", "image", "inspect", IMAGE) || system("docker", "build", "-q", "-t", IMAGE, "tools/gate", out: File::NULL) ||
      abort("gate: docker build of #{IMAGE} failed")
    log = ENV["GATE_LOG"] || File.join(git("rev-parse", "--git-dir"), "gate-linux.log")
    puts "gate: tree #{tree[0, 12]} merged with master #{base[0, 12]} -> #{log}"
    ok = IO.popen(["git", "archive", tree]) do |archive|
      system("docker", "run", "--rm", "-i", "-v", "spinel-gate-cache:/cache", IMAGE, "bash", "-c", CONTAINER,
             in: archive, out: log, err: [:child, :out])
    end
    text = File.read(log)
    puts text.lines.grep(/\A(Tests:|scale-test|gate:)/).last(7)
    abort "gate: failed; see #{log}" unless ok && text.include?("gate: ALL GREEN")

    pass, fail = text.scan(/^Tests: (\d+) pass, (\d+) fail/).last
    write_stamp(tree, base, text[/^@@platform (.+)$/, 1], "#{pass}/#{fail}")
  end
end

if $PROGRAM_NAME == __FILE__
  case ARGV.shift
  when "start" then Gate.start
  when "stamp" then Gate.stamp
  when "trailer" then Gate.trailer(ARGV.fetch(0))
  when "verify" then exit Gate.verify(ARGV[0] || "HEAD")
  when "check" then exit Gate.check
  when "check-range" then exit Gate.check_range(ARGV.fetch(0), ARGV.fetch(1))
  when "linux" then Gate.linux
  else abort "usage: ruby tools/gate.rb start | stamp | trailer MSG | verify [COMMIT] | check | check-range BASE HEAD | linux"
  end
end
