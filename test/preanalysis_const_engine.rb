# spinel: not-cruby -- RUBY_ENGINE is "spinel" under Spinel and "ruby" under CRuby

if RUBY_ENGINE == "spinel"
  puts "engine_spinel"
else
  require "spinel_nonexistent_cruby_only_gem"
  eval("1 + 1")
  dead_eng_local = "never"
end
p [:dead_eng_local, dead_eng_local, defined?(dead_eng_local), binding.local_variable_get(:dead_eng_local)]

unless RUBY_ENGINE == "spinel"
  require "spinel_nonexistent_cruby_only_gem"
  eval("2 + 2")
end

v_nil = if RUBY_ENGINE != "spinel"
  99
end
p [:engine_nil, v_nil]

typed = if RUBY_ENGINE == "spinel" && Object.const_defined?(:RUBY_ENGINE)
  40
else
  "not_an_int"
end
p [:engine_typed, typed + 2]
