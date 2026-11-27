# A require of a computed path in a method of a class no object of which is
# ever made (its initialize always raises, tzinfo's RubyDataSource without
# tzinfo-data) never runs, so the files loaded after it keep their places
# and an alias across two of them reads their order.
require_relative "computed_require_unmade_class/source"
SOURCE = pick_source
require_relative "computed_require_unmade_class/greeter"
require_relative "computed_require_unmade_class/loud"
p SOURCE
p Greeter.new.greet
