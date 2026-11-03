# spinel: reject-subclass: a method defined under a condition only the run time decides is not answered for
# A lone def under a run-time condition in a module stays refused for every
# list of the program: the module may be mixed into Object (here) or into a
# class by a run-time include, which a list's receiver chain does not name.
$on = ENV["SPINEL_TEST_UNSET_VARIABLE"].nil?
module M
  def mm = 1 if !$on
end
class Object
  include M
end
class Other; end
p Other.instance_methods.include?(:mm)
