# spinel: defer-refusals
# spinel: not-cruby -- Spinel refuses Proc#ruby2_keywords and this file defers the refusal to run time; CRuby's ruby2_keywords answers the proc and raises nothing
# A generated file names the source its lines came from (#<SPINEL_SOURCE>);
# that moves the reported position, not the file whose `defer-refusals`
# comment governs the lines.
#<SPINEL_SOURCE>template.erb:12
def normalize(value)
  proc { value }.ruby2_keywords
end

begin
  normalize("a")
rescue NotImplementedError => error
  puts error.class
  puts error.message.include?("ruby2_keywords")
  puts error.message.start_with?("template.erb:12:")
end
