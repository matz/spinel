# A String operand that is a call, held ahead of the nil test of a receiver
# that can be nil: the callee is spliced in and answers its handle, so the
# temp takes the handle too (a bytes temp of it did not compile).
class A
  def f(x)
    return x.to_s * 2 unless block_given?
    yield x.to_s
    nil
  end
end
a = A.new
out = a.f(5)
out << a.f(1)
p out
