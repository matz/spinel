# block params shadowing an outer local at 1, 2 and 3 levels, each level a different type
x = "outer"
[1, 2].each { |x| p x * 2 }
[[1, 2]].each { |x|
  x.each { |y|
    [y.to_s].each { |x| p x + "!" }
  }
}
[3].each { |x|
  [4.5].each { |x|
    [:s].each { |x| p x }
    p x
  }
  p x
}
p x
