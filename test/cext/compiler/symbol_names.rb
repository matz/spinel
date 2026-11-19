# frozen_string_literal: true
require_relative 'symbol_names'

[:hello, :"", :"a\0b", :"café", "dynamic-#{17}".to_sym].each do |symbol|
  name = CextSymbolNames.name_of(symbol)
  p [name.bytes, name.frozen?]
  p name.equal?(CextSymbolNames.name_of(symbol))
  GC.start
  p name.equal?(CextSymbolNames.name_of(symbol))
  begin
    name << "changed"
  rescue FrozenError
    puts "frozen"
  end
  copy = name.dup
  copy << "!"
  p [copy.bytes, CextSymbolNames.name_of(symbol).bytes]
  p [name.equal?(symbol.to_s), symbol.to_s.equal?(symbol.to_s)]
end

# Exercise the other first-caller order and retain the canonical name only
# through the Symbol cache across a collection.
symbol = :"ruby-first\0name"
original = symbol.name
p original.equal?(CextSymbolNames.name_of(symbol))
original = nil
GC.start
p CextSymbolNames.name_of(symbol).equal?(symbol.name)

# Static String literals remain distinct from the Symbol spelling, including
# when an unrelated dynamic dedup causes literal-table registration.
p(-("unrelated-" + 17.to_s))
p CextSymbolNames.name_of(:hello).equal?(-"hello")
p CextSymbolNames.name_of(:"a\0b").equal?(-"a\0b")
p CextSymbolNames.name_of(:hello).equal?(:hello.name)
p CextSymbolNames.name_of(:"café").equal?(:"café".name)
