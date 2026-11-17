class TrueClass
  def to_s = +"yes"
end

def boolean_text(value)
  value.to_s
end

p boolean_text(true), boolean_text(false)
p boolean_text(true).equal?(boolean_text(true))
