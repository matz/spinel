# The cases of class_qualification_leaf_collisions.rb with the definitions in the reverse order,
# so every decision is met in the other order too.
module Solo4; class Only4; def self.id = :only4; end; end
module H; class Item; def self.id = :h; end; end

module G; class Widget; def self.id = :nested; end; end
class Widget; def self.id = :top; end

module E; class Array; def self.id = :e_array; end; end

module Twice; class Reopen; def self.one = 1; end; end
module Twice; class Reopen; def self.two = 2; end; end

module Solo3; class Only3; def self.id = :only3; end; end

module A; class Item; def self.id = :a; end; end
module B; class Item; def self.id = :b; end; end
module C; module Inner; class Item; def self.id = :c_inner; end; end; end

module Solo1; class Only1; def self.id = :only1; end; end
module Solo2; class Only2; def self.id = :only2; end; end

puts [Solo1::Only1, Solo2::Only2, Solo3::Only3, Solo4::Only4].map { |k| "#{k.name} #{k.id}" }
puts [A::Item, B::Item, C::Inner::Item, H::Item].map { |k| "#{k.name} #{k.id}" }
puts "#{Twice::Reopen.name} #{Twice::Reopen.one} #{Twice::Reopen.two}"
puts "#{E::Array.name} #{E::Array.id} #{[1, 2].class} #{Array.new(2).size}"
puts "#{Widget.name} #{Widget.id} #{G::Widget.name} #{G::Widget.id}"
puts [A::Item, B::Item, C::Inner::Item, H::Item].map(&:object_id).uniq.size
