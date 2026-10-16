function probe_timevar_invalid(out)
% A structurally valid but numerically invalid reference combination must reject.
result.matlab=version;result.signal=ver('signal');
result.N=2030;result.fcMinMhz=1e-30;result.p1=780;result.p2=1000;
try
    [y,info]=timevarLowpassSTFT(ones(2030,1),250e6,45e6,1e-24,780,1000,{});
    result.nonfiniteY=any(~isfinite(y));result.nonfiniteH=any(~isfinite(info.H(:)));
    result.minFc=min(info.fc);
    result.rejected=result.nonfiniteY || result.nonfiniteH;
catch ex
    result.rejected=true;result.identifier=ex.identifier;result.error=ex.message;
end
f=fopen(out,'w');fwrite(f,jsonencode(result),'char');fclose(f);
assert(result.rejected);
fprintf('Confirmed MATLAB nonfinite cutoff-cancellation case; production rejects before commit.\n');
end
