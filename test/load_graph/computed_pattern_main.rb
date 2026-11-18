# A require whose path joins literals around a part only the run time
# knows (the fat-binary idiom, `"x/#{$1}/native"`) can name only the files
# of that shape: here none, so it raises LoadError, which the rescue takes.
# Under --load-model=graph the other files keep their place at their own
# require, and a module one of them mixes in is found there, as is the
# method super reaches through it.
begin
  RUBY_VERSION =~ /(\d+\.\d+)/
  require_relative "computed_pattern/#{$1}/native_lib"
rescue LoadError
  require_relative "computed_pattern_native_lib"
end
require_relative "computed_pattern_db_lib"
db = ComputedPatternDb.new
p db.raw
p db.enc
p ComputedPatternDb.ancestors.take(2)
