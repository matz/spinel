# `klass = obj.class` of a receiver of no known class lets no class escape
# when the local is only read where it is consumed: here a case arm's value
# held in another local that only an interpolation reads. Taken as handed on,
# every class escaped, an untyped `@model.new(hash)` bound the Hash into
# CofFlash#initialize, and the Symbol key its each pushed was refused in the
# Array[String] slot (#11610, after #5217 and #5271).
class CofRelation
  def initialize(model)
    @model = model
  end

  def scope_attributes
    {}
  end

  def build(conditions)
    @model.new(scope_attributes.merge(conditions))
  end
end

class CofFlash
  def initialize(other = nil)
    @discard = []
    return if other.nil?
    other.each do |k, v|
      @discard.push(k)
    end
  end

  def size
    @discard.size
  end
end

def cof_describe(obj)
  name = case obj
         when nil then "nil"
         else
           klass = obj.class
           klass
         end
  "no implicit conversion of #{name} into String"
end

puts CofFlash.new.size
puts cof_describe(1)
puts cof_describe(:a)
puts cof_describe(nil)
