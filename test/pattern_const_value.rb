# A constant that holds a value, in a pattern: value === subject.
LIMIT = 3
NAME = "a"
TAG = :t
RATE = 1.5
p [3, 4, 3.0, "a", :t, 1.5, nil].map { |v|
  case v
  in LIMIT then :limit
  in NAME then :name
  in TAG then :tag
  in RATE then :rate
  else :no
  end
}
p(case 3; in LIMIT then :limit; else :no; end)
p(case 4; in LIMIT then :limit; else :no; end)
p(case "a"; in NAME then :name; else :no; end)
p(case "b"; in NAME then :name; else :no; end)
p(case 3; in NAME then :name; else :no; end)
p(case 1.5; in RATE then :rate; else :no; end)
p(case :t; in TAG | LIMIT then :either; else :no; end)
p(case [3, "a"]; in [LIMIT, NAME] then :pair; else :no; end)
