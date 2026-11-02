# spinel: defer-refusals
# spinel: not-cruby -- Spinel refuses unicode_normalize and this file defers the refusal to run time; CRuby normalizes and raises nothing
# A generated file names the source its lines came from (#<SPINEL_SOURCE>);
# that moves the reported position, not the file whose `defer-refusals`
# comment governs the lines.
#<SPINEL_SOURCE>template.erb:12
def normalize(value)
  value.unicode_normalize(:nfc)
end

begin
  normalize("a")
rescue NotImplementedError => error
  puts error.class
  puts error.message.include?("unicode_normalize")
  puts error.message.start_with?("template.erb:12:")
end
