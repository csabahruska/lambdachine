{-# LANGUAGE Unsafe #-}
{-# LANGUAGE NoImplicitPrelude, MagicHash, UnboxedTuples, RankNTypes #-}
{-# OPTIONS_HADDOCK hide #-}
-----------------------------------------------------------------------------
-- |
-- Module      :  GHC.ST
-- Copyright   :  (c) The University of Glasgow, 1992-2002
-- License     :  see libraries/base/LICENSE
--
-- Maintainer  :  cvs-ghc@haskell.org
-- Stability   :  internal
-- Portability :  non-portable (GHC Extensions)
--
-- The 'ST' Monad.
--
-----------------------------------------------------------------------------

module GHC.ST (
        ST(..), STret(..), STRep,
        fixST, runST, runSTRep,

        -- * Unsafe functions
        liftST, unsafeInterleaveST
    ) where

import GHC.Base
--import GHC.Show

default ()

-- | The strict state-transformer monad.
-- A computation of type @'ST' s a@ transforms an internal state indexed
-- by @s@, and returns a value of type @a@.
-- The @s@ parameter is either
--
-- * an uninstantiated type variable (inside invocations of 'runST'), or
--
-- * 'RealWorld' (inside invocations of 'Control.Monad.ST.stToIO').
--
-- It serves to keep the internal states of different invocations
-- of 'runST' separate from each other and from invocations of
-- 'Control.Monad.ST.stToIO'.
--
-- The '>>=' and '>>' operations are strict in the state (though not in
-- values stored in the state).  For example,
--
-- @'runST' (writeSTRef _|_ v >>= f) = _|_@
newtype ST s a = ST (STRep s a)
type STRep s a = State# s -> (# State# s, a #)

instance Functor (ST s) where
    fmap f (ST m) = ST $ \ s ->
      case (m s) of { (# new_s, r #) ->
      (# new_s, f r #) }

instance Monad (ST s) where
    {-# INLINE return #-}
    {-# INLINE (>>)   #-}
    {-# INLINE (>>=)  #-}
    return x = ST (\ s -> (# s, x #))
    m >> k   = m >>= \ _ -> k

    (ST m) >>= k
      = ST (\ s ->
        case (m s) of { (# new_s, r #) ->
        case (k r) of { ST k2 ->
        (k2 new_s) }})

data STret s a = STret (State# s) a

-- liftST is useful when we want a lifted result from an ST computation.  See
-- fixST below.
liftST :: ST s a -> State# s -> STret s a
liftST (ST m) = \s -> case m s of (# s', r #) -> STret s' r

{-# NOINLINE unsafeInterleaveST #-}
unsafeInterleaveST :: ST s a -> ST s a
unsafeInterleaveST (ST m) = ST ( \ s ->
    let
        r = case m s of (# _, res #) -> res
    in
    (# s, r #)
  )

-- | Allow the result of a state transformer computation to be used (lazily)
-- inside the computation.
-- Note that if @f@ is strict, @'fixST' f = _|_@.
fixST :: (a -> ST s a) -> ST s a
fixST k = ST $ \ s ->
    let ans       = liftST (k r) s
        STret _ r = ans
    in
    case ans of STret s' x -> (# s', x #)

-- | Return the value computed by a state transformer computation.
-- The @forall@ ensures that the internal state used by the 'ST'
-- computation is inaccessible to the rest of the program.
runST :: (forall s. ST s a) -> a
runST st = runSTRep (case st of { ST st_rep -> st_rep })

{-# NOINLINE runSTRep #-}
runSTRep :: (forall s. STRep s a) -> a
runSTRep st_rep = case st_rep realWorld# of
                        (# _, r #) -> r
