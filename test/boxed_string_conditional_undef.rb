class String
  if ARGV.empty? || ARGV.include?("undef")
    undef strip
  end

  if ARGV.include?("hook")
    def method_missing(name, *args)
      [:missing, name, args]
    end
  end
end

$events = []

def operand
  $events << :operand
  "x".dup
end

value = [1, nil, "xxhixx"][2]
begin
  p value&.strip(operand)
rescue NoMethodError => error
  p [error.name, error.args, error.receiver.equal?(value)]
end

other = [1, nil, "xxhixx"][1]
p other&.strip(operand)
p $events
