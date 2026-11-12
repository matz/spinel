# A nil-default param keeps nil when a Method object calls the method
# without that argument, even after a direct call passed a Time,
# a Complex or a Rational.
def eq(t = nil) = t.class
eq(Time.at(0))
p method(:eq).call
p method(:eq).()
p method(:eq)[]

def ea(t = nil) = t.class
alias ea2 ea
ea(Time.at(0))
p method(:ea2).call

class K
  def eq(t = nil) = t.class
end
K.new.eq(Time.at(0))
p K.new.method(:eq).call
p K.instance_method(:eq).bind(K.new).call

def ec(c = nil) = c.class
ec(Complex(1, 2))
p method(:ec).call

def er(r = nil) = r.class
er(Rational(1, 2))
p method(:er).call
p er(Rational(3, 4))

def ad0(t = nil) = t.class
alias ad1 ad0
alias ad2 ad1
alias ad3 ad2
alias ad4 ad3
alias ad5 ad4
alias ad6 ad5
alias ad7 ad6
alias ad8 ad7
alias ad9 ad8
alias ad10 ad9
alias ad11 ad10
alias ad12 ad11
ad0(Time.at(0))
p method(:ad12).call
