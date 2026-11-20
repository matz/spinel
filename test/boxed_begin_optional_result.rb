def integer_result(flag)
  value = begin
    begin
      throw :done, 7
    ensure
      p :integer_ensure
      GC.start
    end
  rescue StandardError
    flag ? 7 : nil
  end
  p value
end

def float_result(flag)
  value = begin
    begin
      throw :done, 2.5
    ensure
      p :float_ensure
      GC.start
    end
  rescue StandardError
    flag ? 2.5 : nil
  end
  p value
end

p(catch(:done) { integer_result(true) })
p(catch(:done) { integer_result(false) })
p(catch(:done) { float_result(true) })
p(catch(:done) { float_result(false) })
