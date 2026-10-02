# `Object.include M` mixes M into Object in CRuby. A program that never
# reopens `class Object` compiled it to a NoMethodError at run time; it is
# refused at compile time now, pointing at the `class Object` body form.
module GuardMixin
  MixedConst = :mixed
end
Object.include GuardMixin
puts "missing" unless defined?(MixedConst)
