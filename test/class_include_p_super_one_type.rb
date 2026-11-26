# A class method's bare super into Module#include? from a parameter that
# holds one kind of value, not a module, passes it boxed by its kind, and
# CRuby's TypeError names that kind.
class F; def self.include?(arg) = super; end
class S; def self.include?(arg) = super; end
class Y; def self.include?(arg) = super; end
class I; def self.include?(arg) = super; end
begin; F.include?(2.5); rescue TypeError => e; p e.message; end
begin; S.include?("x"); rescue TypeError => e; p e.message; end
begin; Y.include?(:y); rescue TypeError => e; p e.message; end
begin; I.include?(7); rescue TypeError => e; p e.message; end
