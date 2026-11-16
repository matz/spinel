#!/usr/bin/env ruby
# frozen_string_literal: true
# entry_names_check.rb -- a typed container's store and search entry names are formatted in one place.
#
#   tools/entry_names_check.rb [--staged | --rev REV]
#
# A String Array holds its elements as handles (typed_elem_held), and a Hash of String values its values: the
# entries that store, search or insert a String are the `_e` ones, and the bytes entries (sp_StrArray_push,
# sp_StrArray_set, sp_StrStrHash_set, ...) are deleted. The suffix of an entry is the container row's
# (typed_elem_store_sfx), and emit_elem_entry (codegen_util.c) is the one formatter of the name:
# `sp_<kind>Array_<op><sfx>`. This check keeps a name from being formatted anywhere else.
#
# In the sources under src/ (.c, .h, .inc), outside comments, every string literal (adjacent literals joined)
# is read for a name of the form sp_<kind>Array_<op> / sp_<kind>Hash_<op>, where <kind> is %s, %.*s, a $A / $H
# template slot or a spelled kind (Str, StrStr, IntStr, Int, Poly, ...), and for an operation name that stands
# apart (`"Array_push"`, `"Array_%s"`, `"Hash_"`). A name whose operation is a store or a search (push, set, oset,
# insert, index, rindex, delete, has_value, new_with_default, unshift, with any suffix) or is not known at the
# site (`%s`, or the end of the literal) is a site; a standalone name is listed as `Array_*` when its operation
# is not spelled. A kind that is spelled and holds no String handle (Int, Poly, ...) has no
# deleted entry and is not read. A site outside emit_elem_entry fails unless tools/entry_names.allow lists
# it: its class, file, function, pattern and how many times it occurs there, with the reason it cannot name a
# deleted entry for a String kind. A listed site that no longer occurs, or occurs another number of times, is a
# failure too, so a copied line is not allowed by the original's entry.
#
# The suffix is not followed: that a variable holds the row's answer is not proved, a site is either routed
# through the formatter or listed.
#
# With --staged the sources and the list are read from the index, with --rev REV from that revision, so the
# change judged is the one read. Exit status: 0 clean, 1 a site to route or list (each is printed).
require "open3"
ROOT = File.expand_path("..", __dir__)
mode = ARGV[0]
rev = ARGV[1]
abort "usage: entry_names_check.rb [--staged | --rev REV]" if mode == "--rev" && (rev.nil? || rev.empty?)

def git_show(spec)
  out, st = Open3.capture2("git", "-C", ROOT, "show", spec, binmode: true)
  st.success? ? out : nil
end

def read_tree(mode, rev)
  files = {}
  case mode
  when "--staged", "--rev"
    ref = mode == "--staged" ? ":" : "#{rev}:"
    list = mode == "--staged" ? ["ls-files", "--", "src"] : ["ls-tree", "-r", "--name-only", rev, "--", "src"]
    names, st = Open3.capture2("git", "-C", ROOT, *list)
    abort "entry-names: git #{list.first} failed for #{mode == '--staged' ? 'the index' : rev}" unless st.success?
    names.split("\n").grep(%r{\Asrc/[^/]+\.(c|h|inc)\z}).each do |f|
      files[f] = git_show("#{ref}#{f}") or abort "entry-names: cannot read #{ref}#{f}"
    end
    abort "entry-names: no source under src/ in #{mode == '--staged' ? 'the index' : rev}" if files.empty?
    allow = git_show("#{ref}tools/entry_names.allow") or abort "entry-names: cannot read #{ref}tools/entry_names.allow"
  else
    abort "usage: entry_names_check.rb [--staged | --rev REV]" if mode
    Dir[File.join(ROOT, "src/*.{c,h,inc}")].sort.each { |p| files["src/" + File.basename(p)] = File.binread(p) }
    allow = File.binread(File.join(ROOT, "tools/entry_names.allow"))
  end
  [files, allow.to_s]
end

# C source with its comments blanked (newlines kept), string literals left alone
def strip_comments(text)
  out = +""
  i = 0
  while i < text.size
    c = text[i]
    if c == '"' || c == "'"
      j = i + 1
      j += (text[j] == "\\" ? 2 : 1) while j < text.size && text[j] != c
      out << text[i..j]
      i = j + 1
    elsif text[i, 2] == "/*"
      j = text.index("*/", i + 2) || text.size
      out << text[i...j + 2].gsub(/[^\n]/, " ")
      i = j + 2
    elsif text[i, 2] == "//"
      j = text.index("\n", i) || text.size
      i = j
    else
      out << c
      i += 1
    end
  end
  out
end

# the string literals of a source, adjacent ones joined: [text, first line]
def literals(text)
  res = []
  i = 0
  line = 1
  cur = nil
  gap_ok = false
  while i < text.size
    c = text[i]
    if c == '"'
      j = i + 1
      j += (text[j] == "\\" ? 2 : 1) while j < text.size && text[j] != '"'
      body = text[i + 1...j]
      if cur && gap_ok then cur[0] << body else res << (cur = [+body, line]) end
      line += text[i..j].count("\n")
      i = j + 1
      gap_ok = true
    elsif c == "'"
      j = i + 1
      j += (text[j] == "\\" ? 2 : 1) while j < text.size && text[j] != "'"
      i = j + 1
      cur = nil
      gap_ok = false
    else
      if c == "\n" then line += 1 end
      gap_ok = false unless c =~ /\s/
      i += 1
    end
  end
  res
end

STORE = /\A(push|set|oset|insert|index|rindex|delete|has_value|new_with_default|unshift)/
HSTORE = /\A(set|oset|has_value|new_with_default)/   # a Hash's delete, index and the like take its key, which stays bytes
NAME = /sp_(?<kind>%\.?\*?s|%s|\$[A-Z]|[A-Z][A-Za-z]*?)(?<word>Array|Hash)_(?<op>[a-z_]*)/
APART = /(?<![\w%])(Array|Hash)_(push|set|insert|index|rindex|delete|has_value|new_with_default|unshift)\b/
APART_DYN = /(?<![\w%])(Array|Hash)_(?:%\.?\*?s|\z)/   # a name that stands apart whose operation is not known at the site

def function_at(lines)
  names = Array.new(lines.size)
  cur = "(file)"
  lines.each_with_index do |l, i|
    m = l.match(/\A[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(/)
    cur = m[1] if m && l !~ /;\s*\z/ && l !~ /\A(?:if|for|while|switch|return|else|case|typedef|#)\b/
    names[i] = cur
  end
  names
end

files, allow_text = read_tree(mode, rev)
sites = Hash.new(0)
site_lines = Hash.new { |h, k| h[k] = [] }
files.each do |file, src|
  next unless src
  text = strip_comments(src.dup.force_encoding("BINARY"))
  funcs = function_at(text.split("\n", -1))
  literals(text).each do |lit, line|
    fn = funcs[line - 1] || "(file)"
    next if file == "src/codegen_util.c" && fn == "emit_elem_entry"
    lit.scan(NAME) do
      m = Regexp.last_match
      op = m[:op]
      next unless op.empty? || op =~ (m[:word] == "Hash" ? HSTORE : STORE)
      next unless m[:kind] =~ /\A(%\.?\*?s|\$[A-Z]|Str|StrStr|IntStr)\z/   # a spelled kind that holds no String handle has no deleted entry
      next if m[:kind] =~ /\A(Str|StrStr|IntStr)\z/ && op =~ /_e\z/   # the handle entry itself
      pat = "sp_#{m[:kind]}#{m[:word]}_#{op.sub(/_(e|o|poly|poly_e|nilable|nil|grow|key|bytes)\z/, '')}"
      sites[[file, fn, pat]] += 1
      site_lines[[file, fn, pat]] << line
      warn "#{file}:#{line}: #{fn}: #{pat}" if ENV["ENTRY_NAMES_LINES"]
    end
    lit.scan(APART) { k = [file, fn, "#{$1}_#{$2} (apart)"]; sites[k] += 1; site_lines[k] << line }
    lit.scan(APART_DYN) { k = [file, fn, "#{$1}_* (apart)"]; sites[k] += 1; site_lines[k] << line }
  end
end

allow = {}
allow_text.split("\n").each do |l|
  next if l.empty? || l.start_with?("#")
  cls, file, fn, pat, count, reason = l.split("\t", 6)
  abort "entry_names.allow: bad line: #{l}" unless reason && cls.split("+").all? { |c| %w[NUM POLY ARM READ ROW IDX].include?(c) } && count =~ /\A\d+\z/
  allow[[file, fn, pat]] = [count.to_i, false]
end

bad = []
sites.each do |key, n|
  a = allow[key]
  if a.nil?
    bad << "#{key[0]}:#{site_lines[key].join(',')}: #{key[1]}: #{n} store or search entry name(s) formatted outside emit_elem_entry: #{key[2]} (route them through it, or list them in tools/entry_names.allow with the reason)"
  elsif a[0] != n
    bad << "#{key[0]}: #{key[1]}: #{key[2]} occurs #{n} time(s), tools/entry_names.allow lists #{a[0]}"
    a[1] = true
  else
    a[1] = true
  end
end
allow.each { |key, (n, used)| bad << "tools/entry_names.allow: no such site: #{key.join(' ')}" unless used || sites.key?(key) }
puts bad
puts "entry-names: pass (#{allow.size} listed sites, #{sites.values.sum} name formats)" if bad.empty?
exit(bad.empty? ? 0 : 1)
