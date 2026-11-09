#!/usr/bin/env ruby
# frozen_string_literal: true
# CRuby's own method tables for its builtin classes and modules: for each, the
# names of its own instance methods (public, private and protected), the names
# of its own singleton methods, and its ancestors. src/builtin_names.c answers
# "does CRuby's builtin X have a method m on this side" from them
# (builtin_method_answers), which the hand-written lists of what Spinel
# implements cannot.
# ruby --enable-frozen-string-literal tools/gen_builtin_method_tables.rb --write
# --check regenerates in memory and fails on any drift.
#
# The tables are of the CRuby alone: the gems its own default dir holds and
# no RUBYOPT, as the oracle runs. Each library's gem is taken twice, at the
# version CRuby shipped (its default gem, for a bundled gem the oldest
# installed) and at the newest installed, and the tables are the union: a
# missed NameError is master's answer, a false one is a wrong answer. A
# machine with other gems installed can therefore drift from the checked-in
# file; the check is not part of the gate.
#
# Each name, ancestor and row also carries what must be loaded for it to
# exist: bit 0 when CRuby has it with no library required, bit 1 + i when
# library i (LIBRARIES order) alone supplies it at the shipped version. A reader
# that must be sure a builtin answers (it drops a method_missing or a
# NoMethodError because of the answer) counts a name only when its bits
# hold for the libraries the program requires; one that only asks whether
# one may answer ignores them.
require "rbconfig"

# The libraries a program may require that Spinel answers natively
# (spinel_parse.c: sp_lib_is_native, sp_require_preloaded), and the bundled
# ones it runs as its own code. A LoadError fails the generator.
LIBRARIES = %w[time json date set stringio strscan io/console bigdecimal socket monitor ostruct rbconfig].freeze
GEMS = { "json" => "json", "date" => "date", "stringio" => "stringio", "strscan" => "strscan", "time" => "time",
  "bigdecimal" => "bigdecimal", "ostruct" => "ostruct", "io/console" => "io-console" }.freeze
# The names a program also spells without their namespace.
SHORT = { "Mutex" => "Thread::Mutex", "Queue" => "Thread::Queue", "SizedQueue" => "Thread::SizedQueue",
  "ConditionVariable" => "Thread::ConditionVariable" }.freeze

# One process: the tables of one pinning ("shipped" or "newest") with the
# libraries named (none: CRuby alone), marshalled.
if ARGV.first == "--data"
  libs = ARGV[2].to_s.split(",")
  libs.each do |lib|
    g = GEMS[lib] or next
    stubs = Gem::Specification.stubs_for(g)
    pick = ARGV[1] == "shipped" ? (stubs.find(&:default_gem?) || stubs.min_by(&:version)) : stubs.max_by(&:version)
    abort "no gem #{g}" unless pick
    gem g, "= #{pick.version}"
  end
  libs.each { |lib| require lib }
  inst = ->(m) { (m.instance_methods(false) + m.private_instance_methods(false) + m.protected_instance_methods(false)).map(&:to_s) }
  seen = {}
  walk = lambda do |mod|
    mod.constants(false).each do |n|
      # (an autoload constant is a library CRuby has not loaded: reading it would load it, and its methods
      # on the core classes (Integer#to_bn from openssl) would be read as CRuby's own)
      next if mod.autoload?(n)
      v = (mod.const_get(n, false) rescue nil)
      next unless v.is_a?(Module) && v.name && !seen[v.name] && v.name == (mod == Object ? n.to_s : "#{mod.name}::#{n}")
      seen[v.name] = v
      walk.(v)
    end
  end
  walk.(Object)
  rows = seen.map { |name, m| [name, { module: m.instance_of?(Module), inst: inst.(m), sing: inst.(m.singleton_class), ipub: m.public_instance_methods(false).map(&:to_s),
      spub: m.singleton_class.public_instance_methods(false).map(&:to_s), anc: m.ancestors.map(&:name).compact }] }.to_h
  $stdout.binmode
  $stdout.write Marshal.dump([rows, Gem.loaded_specs.map { |n, sp| [n, sp.version.to_s] }.sort])
  exit
end

module BuiltinMethodTables
  OUT = File.expand_path("../src/builtin_method_tables.inc", __dir__)

  def self.data(pin, libs)
    dir = Gem.default_dir
    env = { "GEM_HOME" => dir, "GEM_PATH" => dir, "RUBYOPT" => nil }
    IO.popen(env, [RbConfig.ruby, "--enable-frozen-string-literal", __FILE__, "--data", pin, libs.join(",")], "rb") { |io| Marshal.load(io.read) }
  end

  def self.c(s) = "\"" + s.gsub(/["\\]/) { |q| "\\" + q } + "\""
  def self.id(n) = n.gsub("::", "__")

  # What must be loaded for `present` (a block on a run's rows) to hold: bit 0
  # with no library, bit 1 + i when library i alone has it at the version CRuby
  # ships: the oracle runs that one, and a library that moves a method between
  # modules from one version to the next (json's to_json) must not lose its
  # ancestors to the newer pinning. The newest run only adds names, at no bit.
  def self.flag(core, libruns, &present)
    f = present.(core) ? 1 : 0
    libruns.each_with_index { |(sh, _), i| f |= 2 << i if present.(sh) }
    f
  end

  # bit 14: public in every run that has the name (a private method answers
  # an implicit-self call alone, and a reader that cannot see the call's form
  # does not count it)
  PUBLIC = 1 << 14
  def self.public?(runs, n, key, pub, x)
    runs.compact.all? { |row| (r = row[n]).nil? || !r[key].include?(x) || r[pub].include?(x) }
  end

  def self.render
    (a, ga), (b, gb) = data("shipped", LIBRARIES), data("newest", LIBRARIES)
    core = data("shipped", []).first
    libruns = LIBRARIES.map { |l| [data("shipped", [l]).first, data("newest", [l]).first] }
    rows = a.merge(b) do |_, x, y|
      { module: x[:module], inst: x[:inst] | y[:inst], sing: x[:sing] | y[:sing], anc: x[:anc] | y[:anc] }
    end
    rows.each_value { |r| r[:inst] = r[:inst].uniq.sort; r[:sing] = r[:sing].uniq.sort }
    gems = ->(g) { g.map { |n, v| "#{n} #{v}" }.join(", ") }
    out = +"/* Generated by tools/gen_builtin_method_tables.rb from CRuby #{RUBY_VERSION}p#{RUBY_PATCHLEVEL}: do not edit.\n"
    out << "   Libraries loaded: #{LIBRARIES.join(' ')}\n"
    out << "   Gems, as shipped: #{gems.(ga)}\n"
    out << "   Gems, newest installed: #{gems.(gb)}\n"
    out << "   Each builtin class or module, the union of the two: its own instance method names\n"
    out << "   (public, private, protected), its own singleton method names and its ancestors,\n"
    out << "   sorted, with their lengths, and for each the libraries it needs: bit 0 none, bit 1 + i\n"
    out << "   library i of BMT_LIBS alone at the shipped version (0: only the newest installed has it, or only libraries loaded together).\n"
    out << "   builtin_method_answers (builtin_names.c) reads them. */\n"
    out << "static const char *const BMT_LIBS[] = { #{LIBRARIES.map { c(_1) }.join(', ')} };\n"
    names = rows.keys.sort
    names.each do |n|
      r = rows[n]
      { "i" => :inst, "s" => :sing, "a" => :anc }.each do |k, key|
        out << "static const char *const bmt_#{id(n)}_#{k}[] = { #{r[key].map { c(_1) }.push('NULL').join(', ')} };\n"
        pub = { inst: :ipub, sing: :spub }[key]
        runs = [core, *libruns.flatten, a, b]
        fl = r[key].map do |x|
          f = flag(core[n], libruns.map { |sh, nw| [sh[n], nw[n]] }) { |row| row && row[key].include?(x) }
          f |= PUBLIC if pub && public?(runs, n, key, pub, x)
          f
        end
        out << "static const unsigned short bmt_#{id(n)}_#{k}f[] = { #{fl.push(0).join(', ')} };\n"
      end
    end
    out << "static const BuiltinMethodTable BUILTIN_METHOD_TABLES[] = {\n"
    (names.map { |n| [n, n] } + SHORT.to_a).sort_by(&:first).each do |name, long|
      r = rows[long]
      i = id(long)
      rf = flag(core[long], libruns.map { |sh, nw| [sh[long], nw[long]] }) { |row| !row.nil? }
      out << "  { #{c(name)}, #{r[:module] ? 1 : 0}, #{rf}, bmt_#{i}_i, bmt_#{i}_if, #{r[:inst].size}, bmt_#{i}_s, bmt_#{i}_sf, #{r[:sing].size}, " \
             "bmt_#{i}_a, bmt_#{i}_af, #{r[:anc].size} },\n"
    end
    out << "};\n"
  end

  def self.main(argv)
    text = render
    if argv.include?("--check")
      abort "builtin method tables drifted; regenerate with --write" unless File.exist?(OUT) && File.read(OUT) == text
      puts "builtin-method-tables-check: ok"
    elsif argv.include?("--write")
      File.write(OUT, text)
    else
      print text
    end
  end
end

BuiltinMethodTables.main(ARGV) if $PROGRAM_NAME == __FILE__
