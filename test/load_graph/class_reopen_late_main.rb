class A; end
p A.respond_to?(:tag)
begin; A.tag; rescue NoMethodError; p :nomethod; end
require_relative "class_reopen_late_lib"
p A.tag
