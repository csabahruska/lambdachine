{-# LANGUAGE GADTs, MultiParamTypeClasses, PatternGuards, BangPatterns #-}
module Lambdachine.Grin.RegAlloc where

import Lambdachine.Ghc.Utils
import Lambdachine.Grin.Bytecode
import Lambdachine.Grin.Analyse
import Lambdachine.Utils
import Lambdachine.Utils.Unique as U
import qualified Lambdachine.Utils.Graph.Base as Gr
import qualified Lambdachine.Utils.Graph.Ops as Gr
import qualified Lambdachine.Utils.Graph.Colour as Gr

import Compiler.Hoopl hiding ( UniqueSet, UniqueMap )
import Data.Maybe ( fromMaybe )
import Data.Vector ( Vector )
import Data.Word ( Word8 )
import qualified Data.Vector as Vec
import qualified Data.Set as S
import qualified Data.Map as M
import Data.Generics.Uniplate.Direct

allocRegs :: GlobalEnv -> String -> [String] -> BCOs
          -> BytecodeModule
allocRegs env mdl_name mdl_imports bcos0 =
  let !bcos = M.map (allocRegsBco env) bcos0 in
  BytecodeModule
    { bcm_name = mdl_name
    , bcm_imports = mdl_imports
    , bcm_bcos = bcos }

allocRegsBco :: GlobalEnv -> BytecodeObject -> BytecodeObject' FinalCode
allocRegsBco _env bco@BcoCon{} = -- this is just silly
  BcoCon{ bcoType = bcoType bco
        , bcoDataCon = bcoDataCon bco
        , bcoFields = bcoFields bco }
allocRegsBco _env bco@BcConInfo{} =
  BcConInfo{ bcoConTag = bcoConTag bco
           , bcoConFields = bcoConFields bco
           , bcoConArgTypes = bcoConArgTypes bco }
allocRegsBco _env bco@BcTyConInfo{} =
  BcTyConInfo{ bcoDataCons = bcoDataCons bco }
allocRegsBco env bco0@BcObject{ bcoCode = code } =
  BcObject{ bcoType = bcoType bco
          , bcoCode = finaliseCode (bcoArity bco0) code'
          , bcoGlobalRefs = bcoGlobalRefs bco
          , bcoConstants = bcoConstants bco
          , bcoFreeVars = bcoFreeVars bco }
 where
   (bco, live_facts) =
     runM $ analyseAndRewriteBCOBwd bco0
              (livenessAnalysis2 env) noFacts
   code' = allocRegsGraph env live_facts (bcoCode bco)

-- | Allocate registers and linearise bytecode.
allocRegsGraph :: GlobalEnv -> FactBase LiveVars -> Graph BcIns O C -> LinearCode
allocRegsGraph env lives g =
   assignRegs env (lineariseCode env lives g)

-- | Finalise step in generating executable bytecode.  Does the
-- following:
--
--   * Remove some redundant instructions (labels and gotos to adjacent
--     instructions).
--
--   * Turn jump labels into absolute addresses.
--
--   * Calculate the frame size (number of registers needed).
--
finaliseCode :: Int -> LinearCode -> FinalCode
finaliseCode arity lc@(LinearCode code0 lives _ labels) =
  FinalCode framesize code
 where
   -- Frame size is determined by the largest register used by the
   -- register allocator.
   framesize = arity `max` (maxreg + 1)

   BcReg maxreg _ =
     Vec.maximum (Vec.map (maximumDflt (BcReg 0 VoidTy) . universeBi) code)

   maximumDflt n [] = n
   maximumDflt _ xs = maximum xs

   code1 = Vec.imap (\offs ins -> (ins, keep offs ins)) code0
   code = Vec.imap (adjust_idx new_labels0)
        . Vec.map fst
        . Vec.filter snd
        $ code1
   code0_len = Vec.length code0

   keep :: Int -> LinearIns -> Bool
   keep _ (Fst _) = False
   keep _ (Mid _) = True
   keep i (Lst lins) = case lins of
       Goto l
         | i + 1 < code0_len, Fst (Label l') <- code0 Vec.! (i + 1)
         -> l /= l'  -- only keep if jump is not to next instruction
       _ -> True

   new_labels0 = fst $ Vec.foldl' calc_offs (M.empty, 0) code1
    where
      calc_offs :: (M.Map Label Int, Int) -> (LinearIns, Bool)
                -> (M.Map Label Int, Int)
      calc_offs (mp, new_idx) (Fst (Label l), keep) =
        (M.insert l new_idx mp, new_idx)
      calc_offs (mp, new_idx) (i, keep) =
        (mp, if keep then new_idx + 1 else new_idx)

   lookup_label new_labels l =
     case M.lookup l new_labels of
       Just x -> x
       Nothing -> error $ "Label not found: " ++ show l
   adjust_idx new_labels i (Mid ins) =
     Mid $ mapLabels (\l -> (lookup_label new_labels l)) ins
   adjust_idx new_labels i (Lst ins) =
     Lst $ mapLabels (\l -> (lookup_label new_labels l)) ins

-- | Contains linear bytecode annotated with liveness info for each graph.
data LinearCode = LinearCode
  { lc_code     :: Vector LinearIns
  , lc_liveIns  :: Vector LiveVars
  , lc_liveOuts :: Vector LiveVars
  , lc_labels   :: M.Map Label Int
  }

instance Pretty LinearCode where
  ppr (LinearCode is ls live_outs lbls) =
    --ppr lbls $+$
    vcat (Vec.toList (Vec.zipWith pp is ls))
   where pp i l = fillBreak 30 (ppr i) <+> ppr l

lineariseCode :: GlobalEnv -> FactBase LiveVars -> Graph BcIns O C -> LinearCode
lineariseCode env live_facts g@(GMany (JustO entry) body NothingO) =
   LinearCode (annotateWithLiveouts env live_ins lin_code)
              live_ins live_outs labels
 where
   lin_code = Vec.fromList $ concat $
                lineariseBlock env live_facts entry :
                map (lineariseBlock env live_facts) body_blocks
   live_ins = liveIns live_facts lin_code
   live_outs = liveOuts live_facts lin_code
   body_blocks = postorder_dfs g  -- excludes entry sequence
   labels = Vec.ifoldl' ins_if_label M.empty lin_code
   ins_if_label :: M.Map Label Int -> Int -> LinearIns -> M.Map Label Int
   ins_if_label m n (Fst (Label l)) = M.insert l (n+1) m
   ins_if_label m _ _ = m
--   lin_block =

-- | Turn a block into a linear list of instructions.
--
-- Annotates various instructions with the live variables.
--
lineariseBlock :: GlobalEnv -> FactBase LiveVars -> Block BcIns e x -> [LinearIns]
lineariseBlock env live_facts blk =
  entry_ins (map Mid (blockToList middles) ++ tail_ins)
 where
   (entry, middles, tail) = blockSplitAny blk
   entry_ins :: [LinearIns] -> [LinearIns]
   entry_ins = case entry of
                 JustC n -> (Fst n :)
                 NothingC -> id
   tail_ins :: [LinearIns]
   tail_ins = case tail of
                JustC (Case ct x targets) ->
                  [Lst (Case ct x $ map (\(tag, _, lbl) ->
                                         (tag, livesAt lbl, lbl))
                                      targets)]
                JustC (Eval l _ r) ->
                  [Lst (Eval l (livesAt l) r)]
                JustC (Call (Just (var, l, _)) fun args) ->
                  [Lst (Call (Just (var, l, livesAt l)) fun args)]
                JustC x -> [Lst x]
                NothingC -> []

   livesAt label = nonVoid env $ fromMaybe S.empty (lookupFact label live_facts)

nonVoid :: GlobalEnv -> LiveVars -> LiveVars
nonVoid env lives = S.filter (not . isVoid env) lives

-- | Calculate the live-in variables at each instruction.
liveIns :: FactBase LiveVars -> Vector LinearIns -> Vector LiveVars
liveIns global_live_outs inss =
  Vec.postscanr' calcLives S.empty inss
 where
   calcLives (Lst ins) live_out = live ins global_live_outs
   calcLives (Mid ins) live_out = live ins live_out
   calcLives (Fst ins) live_out = live ins live_out

liveOuts :: FactBase LiveVars -> Vector LinearIns -> Vector LiveVars
liveOuts global_live_outs inss =
  Vec.prescanr' calcLives S.empty inss
 where
   calcLives (Lst ins) live_out = live ins global_live_outs
   calcLives (Mid ins) live_out = live ins live_out
   calcLives (Fst ins) live_out = live ins live_out

annotateWithLiveouts :: GlobalEnv
                     -> Vector LiveVars -> Vector LinearIns -> Vector LinearIns
annotateWithLiveouts env lives inss = Vec.imap annotate inss
 where
   annotate :: Int -> LinearIns -> LinearIns
   annotate n (Mid (Assign d (Alloc t args _))) =
     Mid (Assign d (Alloc t args (nonVoid env $ lives Vec.! n)))
   annotate n (Mid (Assign d (AllocAp args _))) =
     Mid (Assign d (AllocAp args (nonVoid env $ lives Vec.! n)))
   annotate n i = i

allRegs :: S.Set BcVar
allRegs = S.fromList $ map (\n -> BcReg n VoidTy) [0..255]

-- * Register Allocation

-- We use a graph-colouring register allocator.  While this is less
-- efficient than a linear-scan register allocator, it has certain
-- advantages w.r.t. flexibility.  In particular, eliminating
-- redundant moves are probably handled more easily by using
-- graph-colouring.

newtype FinalReg = R Word8
  deriving Eq

instance Uniquable FinalReg where
  getUnique (R x) = unsafeMkUniqueNS 'F' (fromIntegral x)

instance Pretty FinalReg where
  ppr (R r) = char 'R' <> text (show r)

-- | TODO: Size2 for Double's?
data RegClass = Size1 | PreColoured FinalReg
  deriving Eq

instance Uniquable RegClass where
  getUnique Size1 = unsafeMkUniqueNS 'C' 1
  getUnique (PreColoured (R r)) = unsafeMkUniqueNS 'C' (2 + fromIntegral r)

instance Pretty RegClass where
  ppr Size1 = text "Sz1"
  ppr (PreColoured r) = text "PC:" <> ppr r

classes :: UniqueMap RegClass (UniqueSet FinalReg)
classes = fromListUM $
  (Size1, (fromListUS (map R [0..255]))) :
  [ let r = R n in (PreColoured r, singletonUS r) | n <- [0..20] ]

-- | The register allocator is parameterised over three types:
--
--  1. The type of virtual registers ('BcVar')
--
--  2. The register classes.  This is used to handle overlapping
--     registers or registers for different operand types (e.g.,
--     floating point vs. integer).  We currently only have one
--     register class.
--
--  3. The type of registers we are allocating to ('FinalReg').
--
type IGraph = Gr.Graph BcVar RegClass FinalReg

-- | Allocate registers for the given code sequence.
--
-- The returned linear code satisfies the invariants:
--
--  * Each 'BcVar' is of shape @BcReg n@.
--
--  * Variables that are live at the same time are allocated to
--    different registers (the register allocation invariant).
--
assignRegs :: GlobalEnv -> LinearCode -> LinearCode
assignRegs env lc@(LinearCode code lives live_outs lbls) =
  let !assign1 = colourGraph env (buildInterferenceGraph lc)
      !code' = Vec.map (transformBi assign1) code
  in
    if not (verifyAlloc assign1 lives) then
      error $ "BUG-IN-REGALLOC1\n" ++ pretty env code ++ "\n\n" ++
               pretty env code' ++ "\n\n"
--            ++ pretty ig
     else
       LinearCode code' lives live_outs lbls
 where
   -- An allocation is valid if registers that are live at the same
   -- time are all assigned different colours.
   verifyAlloc assign1 lives =
     Vec.and (Vec.map (\l -> S.size (S.map assign1 l)
                               == S.size l) lives)

colourGraph :: GlobalEnv -> IGraph -> (BcVar -> BcVar)
colourGraph env igraph =
  case Gr.colourGraph env True 0 classes triv spill igraph of
    (igraph', uncoloured, coalesced)
      | nullUS uncoloured -> get_alloc igraph' coalesced
 where
   -- classes =
   --   singletonUM Size1 (fromListUS (map R [0..255]))
   triv Size1 neighbs excls = True
   triv (PreColoured r) _neighbs excls = not (r `memberUS` excls)

   spill gr = error "Cannot spill"

   get_alloc ig co x@(BcVar _ t) = get_alloc' ig co (transType env t) x
   get_alloc ig co x@(BcReg _ ot) = get_alloc' ig co ot x
   get_alloc' ig co ot x =
     case Gr.lookupNode x ig of
       Just n | Just (R r) <- Gr.nodeColour n
         -- We have a register assignment for this node.
         -> {- trace ("COLOUR["++pretty x++"]=>"++show r) $ -}
            case x of
              BcReg n _ -> if fromIntegral n /= r then
                             error $ "Invalid colouring for pre-assigned colour: "
                               ++ pretty env x
                           else x
              _ -> BcReg (fromIntegral r) ot

       Nothing | Just y <- lookupUM x co
         -- This node has been coalesced with another node.  We'll
         -- have to use the right type, though.
         -> {- trace ("COALESCE["++pretty x++"]=>") $ -}
            get_alloc' ig co ot y

buildInterferenceGraph :: LinearCode -> IGraph
buildInterferenceGraph lc@(LinearCode code0 lives live_outs lbls) =
  {- trace (">>>GRAPH:" ++ pretty gr3) -} gr3
 where
   !gr1 = Vec.ifoldl' add_conflicts Gr.newGraph code0
   !gr2 = Vec.foldl' add_coalesces gr1 code0
   !gr3 = fixColours gr2

   -- TODO: Can be optimised by detecting which new nodes are becoming
   -- live and only adding those.  Each call to Gr.addConflicts is
   -- O(n^2) in the size of the lives set.
   add_conflicts :: IGraph -> Int -> LinearIns -> IGraph
   add_conflicts gr idx instr =
     let live_ins = lives Vec.! idx in
     let outs = live_outs Vec.! idx in
     let gr1 = Gr.addConflicts (conv live_ins) (const Size1) gr in
     let defs = defines instr in
     if defs `S.isSubsetOf` outs then gr1 else
       -- Even if the output of this instruction is not live-out
       -- we still need to allocate a register for it.  We can't
       -- just pick any register, but we must pick one that's not
       -- live-out. Hence, we add a conflict between the defines
       -- and all live-out variables.
       Gr.addConflicts (conv (defs `S.union` outs))
         (const Size1) gr1

   defines :: LinearIns -> LiveVars
   defines (Mid mid) = insDefines mid
   defines (Lst lst) = insDefines lst
   defines _ = S.empty

   -- removePreColoured :: [BcVar] -> [BcVar]
   -- removePreColoured = filter notPreColoured
   --  where notPreColoured (BcVar _ _) = True
   --        notPreColoured (BcReg _ _) = False

   conv :: LiveVars -> UniqueSet BcVar
   conv vs = fromListUS (S.toList vs)

   add_coalesces :: IGraph -> LinearIns -> IGraph
   add_coalesces gr (Mid (Assign dst (Move src)))
    | BcVar _ _ <- dst, BcVar _ _ <- src
    = Gr.addCoalesce (src, Size1) (dst, Size1) gr
    | BcVar _ _ <- dst, BcReg r _ <- src
    = Gr.addPreference (dst, Size1) (R (fromIntegral r)) gr
    | BcReg r _ <- dst, BcVar _ _ <- src
    = Gr.addPreference (src, Size1) (R (fromIntegral r)) gr
   add_coalesces gr _ = gr

   -- For BcVars that represent specific registers, assign the proper
   -- colour.
   fixColours :: IGraph -> IGraph
   fixColours gr =
     Gr.modifyGraphMap gr $ \mp ->
       mapUM fix_colour mp

   fix_colour node =
     case Gr.nodeId node of
       BcReg n _ -> node{ Gr.nodeClass = PreColoured (R (fromIntegral n)) }
       _ -> node

-- | The linear-scan register allocator.
mkAllocMap :: GlobalEnv -> LinearCode -> LinearCode -- M.Map BcVar BcVar
mkAllocMap env lc@(LinearCode code0 lives liveouts lbls) =
  let (alloc, _, _) = Vec.foldl' alloc1 (M.empty, allRegs, S.empty) lives
      code' = assignRegs alloc code0
  in
    if not (verifyAlloc alloc lives) then
      let lc' = LinearCode code' (Vec.map (S.map (alloc M.!)) lives) liveouts lbls in
      error ("BUG-IN-REGALLOC2\n" ++ pretty env lc ++ "\n\n"
             ++ pretty env alloc ++ "\n" ++ pretty env lc')
     else
       LinearCode code' lives liveouts lbls
 where
   verifyAlloc alloc lives =
     Vec.and (Vec.map (\l -> S.size (S.map (alloc M.!) l) == S.size l) lives)
   assignRegs alloc code =
     Vec.map (transformBi (alloc M.!)) code
   alloc1 (alloc, avail0, prev_live) live =
     let freed_vars = prev_live `S.difference` live
         avail1 = (S.map (alloc M.!) freed_vars) `S.union` avail0
         alloc'd_vars = live `S.difference` prev_live
         (alloc', avail') = S.fold alloc2 (alloc, avail1) alloc'd_vars
     in (alloc', avail', live)
   alloc2 x st@(alloc, avail)
     | Just r <- M.lookup x alloc = (alloc, S.delete r avail)
     | otherwise =
       case x of
         BcReg r _ -> (M.insert x x alloc, S.delete x avail)
         _ ->
           let (reg, avail') = S.deleteFindMin avail in
           (M.insert x reg alloc, avail')
