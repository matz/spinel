def capture_yield
  values = [:marker]
  values << yield
  values.last
end

def throw_through_yield(word)
  catch(:done) do
    capture_yield do
      begin
        throw :done, word
      ensure
        puts :ensure
        GC.start
      end
    end
  end
end

word = +"mutable"
result = throw_through_yield(word)
p result.equal?(word)
p result << "!"

def throw_through_short_circuit(word)
  catch(:done) do
    (puts :prefix; begin
      throw :done, word
    ensure
      puts :condition_ensure
      GC.start
    end) && (puts :right_operand; :other)
  end
end

p throw_through_short_circuit(word).equal?(word)
p throw_through_short_circuit(false)

def throw_from_case(flag, word)
  catch(:done) do
    case flag
    when true
      begin
        throw :done, word
      ensure
        puts :case_ensure
        GC.start
      end
    else
      :skip
    end
  end
end

p throw_from_case(false, word)
p throw_from_case(true, word).equal?(word)

def throw_from_match(value)
  catch(:done) do
    case value
    in [0]
      begin
        throw :done, 7
      ensure
        puts :match_ensure
        GC.start
      end
    else
      1
    end
  end
end

p throw_from_match([1])
p throw_from_match([0])
