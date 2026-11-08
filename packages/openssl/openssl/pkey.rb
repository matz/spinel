# OpenSSL::PKey::EC -- elliptic-curve keys, in the shape #4221 settled on:
# CRuby's names where CRuby has a spelling, raw bytes where CRuby would hand
# back a BN or an EC::Point.
#
# So `EC.generate("prime256v1")` and `key.dh_compute_key(peer)` are the names
# a CRuby program already writes, but a key's halves are Strings --
# `private_key_bytes` is the scalar, `public_key_bytes` is the X9.62
# uncompressed point, 0x04 || X || Y -- and dh_compute_key takes the peer's
# bytes rather than a Point built out of a BN. `EC::Group` supplies curve_name;
# `OpenSSL::BN` and `EC::Point` are not here, and a program that builds one
# fails at COMPILE time the way the rest of this package's gaps do.
#
# That is a real subset, and it is a deliberate one: those arithmetic classes exist
# in CRuby to express things this runtime has no other use for, and every
# protocol that names raw EC keys -- RFC 8291 Web Push, JWK's x/y halves,
# WebAuthn, ES256 -- names them as the byte strings above. A program that
# reaches for EC::Point is already doing protocol work that has to know the
# encoding anyway.
#
# A key is its bytes, so nothing on the C side outlives a call: there is no
# handle to release and no slot to run out of, unlike the connection table
# SSLSocket holds. An abandoned key costs nothing.
module OpenSSL
  module PKey
    class PKeyError < OpenSSLError
    end

    ECError = PKeyError

    class EC
      class Group
        # Curve metadata only; group arithmetic is outside this subset.
        class Error < OpenSSLError; end
        def curve_name = @curve_name.dup
        def initialize(curve)
          curve = curve.is_a?(Group) ? curve.curve_name : OpenSSL.send(:comparison_string, curve)
          raise ArgumentError, "string contains null byte" if curve.include?("\0")
          @curve_name = Native.ec_curve_name(curve, 0).b
          raise Error, "unknown curve name" if @curve_name.empty?
        end
      end

      # The curve is carried, not inferred: the byte lengths of both halves
      # follow from it, and a key whose curve is implicit has to be guessed at
      # from a length the moment two curves are in play.
      attr_reader :curve
      attr_reader :private_key_bytes
      attr_reader :public_key_bytes

      # CRuby's spelling, and CRuby's argument: the curve's name, "prime256v1"
      # for P-256. Whatever OBJ_txt2nid resolves works; anything else raises.
      def self.generate(curve)
        priv = Native.ec_generate(curve)
        raise ECError, Native.last_error if priv.empty?
        new(curve, priv)
      end

      # No CRuby spelling: CRuby reads a key from DER or PEM, which PKey.read
      # below does. The bytes are what a protocol stores -- a VAPID
      # application server key is exactly this scalar, base64url'd.
      def self.from_private_bytes(curve, bytes)
        new(curve, bytes)
      end

      # A public-only key: everything a verifier has. #verify_raw works on one,
      # and #dh_compute_key and #sign_raw raise, because there is no scalar to
      # do them with. The point is checked here rather than at first use, so a
      # peer key that is not on the curve is rejected where it arrives.
      def self.from_public_bytes(curve, bytes)
        new(curve, "", bytes)
      end

      # An empty private half means "public only"; the two are never both
      # absent, and the private one wins when both are given, since the public
      # half derives from it.
      def initialize(curve, private_key_bytes, public_key_bytes = "", point_conversion_form = 4, encoding_flags = 0)
        @curve = curve
        @point_conversion_form = point_conversion_form
        @encoding_flags = encoding_flags
        @private_key_bytes = private_key_bytes
        if private_key_bytes.empty?
          raise ECError, "a key needs a private scalar or public bytes" if public_key_bytes.empty?
          @public_key_bytes = public_key_bytes
          # The point is checked where it arrives, not at first use: an ECDSA
          # verify against something that is not on the curve must not be
          # reachable at all.
          if Native.ec_check_point(curve, public_key_bytes) != 1
            raise ECError, Native.last_error
          end
        else
          @public_key_bytes = Native.ec_public_bytes(curve, private_key_bytes)
          raise ECError, Native.last_error if @public_key_bytes.empty?
        end
      end

      # True for a key built from public bytes alone.
      def public_only?
        @private_key_bytes.empty?
      end

      # CRuby's predicates, and CRuby's aliases for them. A key here always
      # has its public half, derived from the scalar or given as the point, so
      # public? is true for every key; CRuby's EC.new(curve) makes a key with
      # neither half, which this package has no way to build.
      def private?
        !public_only?
      end

      def public?
        true
      end

      alias private_key? private?
      alias public_key? public?

      # CRuby's name; CRuby takes an EC::Point and this takes the same point's
      # uncompressed bytes. The answer is identical either way: the X
      # coordinate of the product, field-width, which is what CRuby returns
      # when no KDF is asked for.
      #
      # A peer key that is not a point on this curve raises rather than
      # answering: an ECDH against an off-curve point leaks the private scalar
      # to whoever chose it.
      def dh_compute_key(peer_public_bytes)
        need_private!("dh_compute_key")
        secret = Native.ec_dh(@curve, @private_key_bytes, peer_public_bytes)
        raise ECError, Native.last_error if secret.empty?
        secret
      end

      # ECDSA, as the raw `r || s` every JOSE and WebAuthn context means by a
      # signature -- two field-width integers, 64 bytes on P-256.
      #
      # NOT SPELLED #sign, and the difference is the point. CRuby's #sign
      # answers DER, and a DER signature is a perfectly well-formed String
      # that every JWT verifier rejects: keeping CRuby's name for bytes CRuby
      # does not produce would compile a CRuby program here and fail it
      # remotely, in someone else's stack, long after the call. The argument
      # shape IS CRuby's -- a digest name and the data, not a pre-computed
      # hash -- so a caller cannot accidentally sign something unhashed.
      def sign_raw(digest, data)
        need_private!("sign_raw")
        sig = Native.ecdsa_sign(@curve, @private_key_bytes, hash_of(digest, data), 0)
        raise ECError, Native.last_error if sig.empty?
        sig
      end

      # CRuby's #verify argument order, over the same raw signature. Answers
      # false for a signature that does not verify AND for one that is
      # malformed: a caller asking "is this good" must not be able to read
      # "the input was strange" as yes.
      def verify_raw(digest, signature, data)
        Native.ecdsa_verify(@curve, @public_key_bytes, hash_of(digest, data), signature, 0) == 1
      end

      def group = Group.new(Native.ec_curve_name(@curve, 1))
      def public_to_der = export_key(0)
      def private_to_der = export_key(1)
      def private_to_pem = export_key(2)

      # The caller has already hashed the data; libcrypto owns DER encoding.
      def dsa_sign_asn1(digest)
        digest = OpenSSL.send(:comparison_string, digest)
        need_private!("dsa_sign_asn1")
        signature = Native.ecdsa_sign(@curve, @private_key_bytes, digest, 1)
        raise ECError, Native.last_error if signature.empty?
        signature
      end

      def dsa_verify_asn1(digest, signature)
        result = Native.ecdsa_verify(@curve, @public_key_bytes,
                                    OpenSSL.send(:comparison_string, digest), OpenSSL.send(:comparison_string, signature), 1)
        raise PKeyError, "invalid DER signature" if result < 0
        result == 1
      end

      private

      def export_key(format)
        need_private!("private export") if format != 0
        bytes = Native.ec_export(@curve, @private_key_bytes, @public_key_bytes,
                                 format, @point_conversion_form, @encoding_flags)
        raise ECError, Native.last_error if bytes.empty?
        bytes
      end

      def need_private!(what)
        raise ECError, "#{what} needs a private key; this one is public only" if public_only?
      end

      def hash_of(digest, data)
        case digest.to_s.upcase
        when "SHA256" then Digest::SHA256.digest(data)
        when "SHA1"   then Digest::SHA1.digest(data)
        else raise Digest::DigestError, "unsupported digest algorithm: #{digest}"
        end
      end

    end

    # CRuby's spelling, for EC and RSA keys. A private EC key in PEM or DER,
    # SEC1 ("EC PRIVATE KEY") or PKCS#8 ("PRIVATE KEY", the form certbot
    # writes), answers a PKey::EC with both halves; a public key ("PUBLIC
    # KEY") answers one that is public only. The C reads the key and hands
    # back its curve and bytes, so the EC is the same object
    # EC.from_private_bytes builds. An RSA key, PKCS#1 or PKCS#8, private or
    # public, answers a PKey::RSA the same way.
    #
    # Any other kind of key raises a PKeyError that names EC and RSA as the
    # kinds this reads, where CRuby would answer a PKey::DSA or DH. There is
    # no passphrase argument, so an encrypted PEM is a key that does not
    # parse.
    def self.read(data)
      blob = Native.pkey_read(data)
      raise PKeyError, Native.last_error if blob.empty?
      return RSA.from_read(blob) if blob.getbyte(0) >= 2
      curve_len = blob.getbyte(1)
      curve = blob.byteslice(4, curve_len)
      bytes = blob.byteslice(4 + curve_len, blob.bytesize - 4 - curve_len)
      if blob.getbyte(0) == 1
        EC.new(curve, bytes, "", blob.getbyte(2), blob.getbyte(3))
      else
        EC.new(curve, "", bytes, blob.getbyte(2), blob.getbyte(3))
      end
    end

    # Raw Ed25519 seeds are copied, like keys imported through PKey.read.
    def self.new_raw_private_key(algorithm, bytes)
      algorithm = OpenSSL.send(:comparison_string, algorithm)
      raise ArgumentError, "string contains null byte" if algorithm.include?("\0")
      bytes = String.new(OpenSSL.send(:comparison_string, bytes))
      raise PKeyError, "unsupported raw private key algorithm" unless algorithm.upcase == "ED25519"
      raise PKeyError, "Ed25519 private key must be 32 bytes" unless bytes.bytesize == 32
      key = PKey.allocate
      key.instance_variable_set(:@seed, bytes)
      key
    end

    # Signing from a raw Ed25519 seed; other PKey algorithms use EC or RSA.
    class PKey
      def initialize
        raise TypeError, "PKey is an abstract class"
      end

      def sign(digest, data)
        raise RuntimeError, "PKey is not initialized" if @seed.nil?
        unless digest.nil? || digest.is_a?(OpenSSL::Digest)
          name = OpenSSL.send(:comparison_string, digest)
          raise ArgumentError, "string contains null byte" if name.include?("\0")
          Native.digest_bytes(name, "")
        end
        data = OpenSSL.send(:comparison_string, data)
        raise PKeyError, "Ed25519 does not accept a digest" unless digest.nil?
        signature = Native.ed25519_sign(@seed, data)
        raise PKeyError, Native.last_error if signature.empty?
        signature
      end

      def verify(digest, signature, data)
        raise RuntimeError, "PKEY wasn't initialized!" if @seed.nil?
        md = 0
        unless nil.equal?(digest)
          if OpenSSL::Digest === digest
            raise RuntimeError, "Digest CTX wasn't initialized!" if nil.equal?(digest.instance_variable_get(:@state))
            name = digest.instance_variable_get(:@name)
          else
            name = String.new(OpenSSL.send(:comparison_string, digest))
          end
          raise ArgumentError, "string contains null byte" if name.include?("\0")
          md = Native.verify_digest(name)
        end
        begin
          signature = OpenSSL.send(:comparison_string, signature)
          data = OpenSSL.send(:comparison_string, data)
          result = Native.ed25519_verify(@seed, signature, data, md)
          raise PKeyError, Native.last_error if result < 0
          result == 1
        ensure
          Native.verify_digest_free(md)
        end
      end
    end
  end
end

module OpenSSL
  module PKey
    # CRuby's openssl 4.0 has no class of its own here: RSAError is the
    # constant PKeyError, so `rescue OpenSSL::PKey::RSAError` and
    # `rescue OpenSSL::PKey::PKeyError` catch the same errors.
    RSAError = PKeyError

    # OpenSSL::PKey::RSA. A public key is what a verifier holds, built from
    # the modulus and exponent bytes, the halves a COSE key or a JWK names it
    # by. There is no CRuby spelling for that (CRuby builds one from DER or
    # PEM), so the constructor has its own name, as EC.from_public_bytes
    # does. A private key comes from PKey.read, and holds its private half as
    # the PKCS#1 DER it was read into, the CRT components the C reads back
    # for each signature; nothing on the C side outlives the call. Signing
    # and verifying use CRuby's names and arguments, and answer what CRuby
    # answers.
    class RSA
      attr_reader :n_bytes
      attr_reader :e_bytes

      def self.from_public_components(n_bytes, e_bytes)
        new(n_bytes, e_bytes)
      end

      # Not CRuby's: PKey.read's answer for an RSA key, a byte for the half
      # (3 private, 2 public), the modulus and the exponent each after a
      # 4-byte length, and for a private key its PKCS#1 DER.
      def self.from_read(blob)
        nl = blob.byteslice(1, 4).unpack1("N")
        n = blob.byteslice(5, nl)
        el = blob.byteslice(5 + nl, 4).unpack1("N")
        e = blob.byteslice(9 + nl, el)
        at = 9 + nl + el
        der = blob.getbyte(0) == 3 ? blob.byteslice(at, blob.bytesize - at) : ""
        new(n, e, der)
      end

      def initialize(n_bytes, e_bytes, private_der = "")
        raise PKeyError, "an RSA public key needs a modulus and an exponent" if n_bytes.empty? || e_bytes.empty?
        @n_bytes = n_bytes
        @e_bytes = e_bytes
        @private_der = private_der
      end

      def public?
        true
      end

      def private?
        !@private_der.empty?
      end

      # CRuby's: the public half, as a key of its own, over copies of the
      # modulus and exponent, so changing the one it answers leaves this
      # key's verifying half alone.
      def public_key
        RSA.new(@n_bytes.dup, @e_bytes.dup)
      end

      # CRuby's: a private key as its PKCS#1 DER, the RSA PRIVATE KEY form,
      # and a public one as the SubjectPublicKeyInfo DER a PUBLIC KEY PEM
      # holds.
      def to_der
        return @private_der.dup if private?
        der = Native.rsa_public_der(@n_bytes, @e_bytes)
        raise PKeyError, Native.last_error if der.empty?
        der
      end

      # PKCS#1 v1.5 over the named digest of the data, as CRuby's PKey#sign:
      # ArgumentError without the private half, and DigestError for an
      # algorithm OpenSSL does not know.
      def sign(digest, data)
        rsa_sign(digest_name(digest), data, 0, "", 0)
      end

      # PSS with MGF1, both keywords required, as in CRuby. salt_length is
      # :digest (the digest's length), :max (the most that fits) or an
      # Integer; anything else is CRuby's TypeError.
      def sign_pss(digest, data, salt_length:, mgf1_hash:)
        salt = case salt_length
               when :digest then -1 # RSA_PSS_SALTLEN_DIGEST
               when :max then -3    # RSA_PSS_SALTLEN_MAX
               when Integer then salt_length
               else raise TypeError, "no implicit conversion of #{salt_length.class} into Integer"
               end
        rsa_sign(digest_name(digest), data, 1, digest_name(mgf1_hash), salt)
      end

      # PKCS#1 v1.5 over the named digest of the data: true or false, and
      # DigestError for an algorithm OpenSSL does not know, as in CRuby.
      def verify(digest, signature, data)
        md = digest_name(digest)
        answer(Native.rsa_verify(@n_bytes, @e_bytes, md, data, signature, 0, md, 0), md, md)
      end

      # PSS with MGF1. Both keywords are required, as in CRuby. salt_length
      # is :auto (read from the signature), :digest (the digest's length) or
      # an Integer; anything else is CRuby's TypeError.
      def verify_pss(digest, signature, data, salt_length:, mgf1_hash:)
        salt = case salt_length
               when :auto then -2   # RSA_PSS_SALTLEN_AUTO
               when :digest then -1 # RSA_PSS_SALTLEN_DIGEST
               when Integer then salt_length
               else raise TypeError, "no implicit conversion of #{salt_length.class} into Integer"
               end
        md = digest_name(digest)
        mgf1 = digest_name(mgf1_hash)
        answer(Native.rsa_verify(@n_bytes, @e_bytes, md, data, signature, 1, mgf1, salt), md, mgf1)
      end

      private

      # CRuby takes the algorithm as its name or as an OpenSSL::Digest.
      def digest_name(digest)
        digest.is_a?(OpenSSL::Digest) ? digest.name : digest.to_s
      end

      def rsa_sign(md, data, pss, mgf1, salt)
        raise ArgumentError, "private key is needed" unless private?
        sig = Native.rsa_sign(@private_der, md, data, pss, mgf1, salt)
        if sig.empty?
          why = Native.last_error
          raise OpenSSL::Digest::DigestError, why if why.start_with?("unsupported digest algorithm")
          raise PKeyError, why
        end
        sig
      end

      # The native call answers 1 for a good signature, 0 for a bad one, -1
      # when the digest name resolves to nothing and -2 for the MGF1 one.
      def answer(rc, md, mgf1)
        raise OpenSSL::Digest::DigestError, "unsupported digest algorithm: #{md}" if rc == -1
        raise OpenSSL::Digest::DigestError, "unsupported digest algorithm: #{mgf1}" if rc == -2
        rc == 1
      end
    end
  end
end
