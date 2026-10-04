# A private method of a module stays private on the object that extends it:
# CRuby raises NoMethodError for an outside call and runs a call on self.
# Spinel copied the method to the module side as public, both for a plain
# `extend P` and for `include P` with `extend self`.

module P
  def shown = "shown " + secret
  private def secret = "secret"
end

module Plain
  extend P
  def self.inside = secret
end

module Both
  include P
  extend self
  def inside = secret
end

p Plain.shown
p Plain.inside
begin
  Plain.secret
rescue NoMethodError => e
  puts e.message
end

p Both.shown
p Both.inside
begin
  Both.secret
rescue NoMethodError => e
  puts e.message
end

# protected stays protected, and a subclass inherits the visibility
module Q
  def via = "via " + guarded.to_s
  protected def guarded = 1
end
class Base
  extend Q
end
class Sub < Base; end

p Base.via
begin
  Base.guarded
rescue NoMethodError => e
  puts e.message
end
begin
  Sub.guarded
rescue NoMethodError => e
  puts e.message
end
p Sub.respond_to?(:guarded)

# the class's own public method of the same name stays public, before or
# after the extend, and its super reaches the private copy
class OwnBefore
  def self.secret = "own " + super
  extend P
end
class OwnAfter
  extend P
  def self.secret = "own " + super
end
p OwnBefore.secret
p OwnAfter.secret

# the later extend wins, whichever of the two is private
module Pub
  def secret = "public"
end
class PrivThenPub
  extend P
  extend Pub
end
class PubThenPriv
  extend Pub
  extend P
end
p PrivThenPub.secret
begin
  PubThenPriv.secret
rescue NoMethodError => e
  puts e.message
end

# a visibility call in the class body wins over the extend
class MadePublic
  extend P
  public_class_method :secret
end
p MadePublic.secret

# the class body's call wins over every extend, also a later one, and a
# call that names another method leaves the copy private
module P2
  private def secret = "secret2"
end
class TwicePublic
  extend P
  extend P2
  public_class_method :secret
end
p TwicePublic.secret

class MadePrivate
  extend Pub
  private_class_method :secret
end
begin
  MadePrivate.secret
rescue NoMethodError => e
  puts e.message
end

class OtherDeclared
  def self.other = 1
  public_class_method :other
  extend P
end
p OtherDeclared.other
begin
  OtherDeclared.secret
rescue NoMethodError => e
  puts e.message
end

# a subclass's own method of the same name has its own visibility, whatever
# the parent's copy or declaration says
class Parent
  extend P
end
class OwnChild < Parent
  def self.secret = "own"
end
class SuperChild < Parent
  def self.secret = "own " + super
end
class DeclParent
  def self.secret = "parent"
  private_class_method :secret
end
class DeclChild < DeclParent
  def self.secret = "own"
end
p OwnChild.secret
p SuperChild.secret
p DeclChild.secret

# a module_function method that an extend copies is private on the class
module Fn
  module_function
  def helper = "helper"
end
class UsesFn
  extend Fn
  def self.run = helper
end
p UsesFn.run
begin
  UsesFn.helper
rescue NoMethodError => e
  puts e.message
end
