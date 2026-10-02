# The same through a constant that holds Object (follow-up to #6905): the
# include is Object's, so it is refused as `Object.include` is, rather than
# raising NoMethodError at run time.
AliasObject = Object
module GuardMixin
  MixedConst = :mixed
end
AliasObject.include GuardMixin
puts "missing" unless defined?(MixedConst)
