LaterLambda = lambda { |value|
  begin
    return value.dup
  ensure
    puts value.dup.upcase
  end
}

LaterNestedLambda = lambda { |xs|
  xs.each { |value| return value if value.even? }
  :none
}
