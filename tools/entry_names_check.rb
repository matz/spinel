#!/usr/bin/env ruby
# frozen_string_literal: true
# entry_names_check.rb -- a typed container's entry names come from its row.
#
#   tools/entry_names_check.rb
#
# A String Array holds its elements as handles (typed_elem_held), and a Hash of String values its values: the
# entries that store, search or insert a String are the `_e` ones, and the bytes entries (sp_StrArray_push,
# sp_StrArray_set, sp_StrStrHash_set, ...) are deleted. The suffix of an entry is the container row's answer,
# typed_elem_store_sfx (emit_array_entry / emit_hash_entry in codegen_util.c format a name from it). This check
# keeps a name that could be a deleted one from being written back by hand. In src/*.c, outside comments, it
# fails when
#   1. a deleted entry is spelled out (sp_StrArray_push( and kin), or
#   2. a format string builds the name of a store or a search (`sp_%sArray_push`, `sp_%sHash_set`, a `%s`
#      operation, ...) in a statement that
#        - takes its suffix from the row (typed_elem_store_sfx, elem_push_sfx, array_push_sfx, emit_kind_push, emit_array_entry,
#          emit_hash_entry, emit_typed_elem_hold, emit_lifted, copy_entry_get) is fine; or
#        - is listed in tools/entry_names.allow with the class and the reason it cannot name a deleted entry
#          for a String kind (a numeric kind, a Poly value, a String arm taken before it).
# Reads (get, oget, vget, get_e, ...) are not deleted names and are not asked about. An allow line whose site no
# longer exists is also a failure, so the list stays what the sources are.
#
# Exit status: 0 clean, 1 a site to route or list.
ROOT = File.expand_path("..", __dir__)
DELETED = /\bsp_(StrArray_(push|set|insert|index|rindex|delete|index_poly|rindex_poly)|(StrStr|IntStr)Hash_(set|has_value|new_with_default))\s*\(/
AOPS = "push|set|insert|index|rindex|delete|index_poly|rindex_poly|unshift"
HOPS = "set|has_value|new_with_default"
FORMAT = /sp_%sArray_(#{AOPS})(%s|\b)|sp_%sHash_(#{HOPS})(%s|\b)|sp_%s(Array|Hash)_%s/
ROW = /typed_elem_store_sfx|elem_push_sfx|array_push_sfx|emit_kind_push|emit_array_entry|emit_hash_entry|emit_typed_elem_hold|emit_lifted|copy_entry_get/
NUMERIC_OP = /_o\b|_nilable|push_nil|_nil\b|\boset\b|\boget\b|\bvget\b/

def strip_comments(text)
  out = +""
  i = 0
  in_str = false
  while i < text.size
    c = text[i]
    if in_str
      out << c
      if c == "\\" then out << text[i + 1].to_s; i += 1
      elsif c == '"' then in_str = false end
    elsif c == '"' then in_str = true; out << c
    elsif text[i, 2] == "/*"
      j = text.index("*/", i + 2) || text.size
      out << text[i...j + 2].gsub(/[^\n]/, " ")
      i = j + 1
    elsif text[i, 2] == "//"
      j = text.index("\n", i) || text.size
      i = j - 1
    else out << c end
    i += 1
  end
  out
end

# the text up to the first `;` outside a string literal
def statement(text, from)
  in_str = false
  k = 0
  while k < text.size
    c = text[k]
    if in_str
      if c == "\\" then k += 1 elsif c == '"' then in_str = false end
    elsif c == '"' then in_str = true
    elsif c == ";" && k >= from then return text[0..k]
    end
    k += 1
  end
  text
end

allow = {}
File.readlines(File.join(ROOT, "tools/entry_names.allow"), chomp: true).each do |l|
  next if l.empty? || l.start_with?("#")
  cls, file, site, reason = l.split("\t", 4)
  abort "entry_names.allow: bad line: #{l}" unless reason && %w[NUM POLY ARM READ].include?(cls)
  allow[[file, site]] = false
end

bad = []
Dir[File.join(ROOT, "src/*.c")].sort.each do |path|
  file = "src/" + File.basename(path)
  lines = strip_comments(File.read(path, encoding: "BINARY")).split("\n")
  lines.each_with_index do |l, i|
    bad << "#{file}:#{i + 1}: spells a deleted entry: #{l.strip[0, 100]}" if l =~ DELETED
    next unless l =~ FORMAT
    joined = lines[i, 8].join(" ")
    stmt = statement(joined, joined.index(FORMAT) || 0)
    next if stmt =~ ROW
    # a suffix held in a variable: its assignment, in the function above, takes the row's answer
    vars = stmt.scan(/(?<![%\w])(\w*sfx|ms|cs|vs|ns|hs)\b(?!\s*\()/).flatten.uniq - ["typed_elem_store_sfx", "nil_store_sfx"]
    next if !vars.empty? && vars.all? { |v| lines[[i - 90, 0].max, 90 + 8].any? { |u| u =~ /\b#{v}\s*=[^=]/ && u =~ ROW } }
    site = l.strip
    if allow.key?([file, site]) then allow[[file, site]] = true; next end
    bad << "#{file}:#{i + 1}: entry name formatted by hand (take the suffix from the row, or list it in tools/entry_names.allow): #{site[0, 100]}"
  end
end
allow.each { |k, used| bad << "tools/entry_names.allow: no such site: #{k[0]}: #{k[1][0, 80]}" unless used }
puts bad
puts "entry-names: pass (#{allow.size} listed sites)" if bad.empty?
exit(bad.empty? ? 0 : 1)
