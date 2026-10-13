clc;clearvars;close all;
set(0,'DefaultLineLineWidth',1.5);set(0,'DefaultAxesLineWidth',1.5);
set(0,'DefaultAxesFontSize',15);set(0,'DefaulttextFontSize',15);
set(0,'DefaultFigureWindowStyle','normal');


% % % %================================================================参数设置=======================================================
%导入参数
FolderPath = 'D:\zzx\Data\20260930\';
FolderNum = [059];
TriggerNum = 4000;%单圈设计触发数
% Invalidtrigger = [];%无效触发数 逐FolderNum对应
ChannelNum = [3]; %通道编号
SampleDepth = 2000; %采样深度 点数
FileName_custom = '_test_'; %自定中间名
FileNum = 5; %文件数量
Num_of_scan = 1;%单轮扫描帧数

%扫描参数
r_0 = 7.0e-3; %探头内重建时，中间填充0的半径
sensor_radius = [6.8].*1e-3;    % DBR旋转半径y6.8 72μm  b6.4 57μm 逐通道编号对应
DAQ = 250*1e6;  % [MHz]
space_n = 1;  %系数越小，传感器数量越多
stepsize = 20e-6 ;    %实际步长与系数相乘[m]
r_water= 8e-3;   %根据组织凹陷进探头内的距离，决定探头内声速为水的半径%04 12%05 11.5
% sysDelay = [476,495;486,505];
sysDelay = [760,775];  %750,767;


y = 28e-3;  %横向宽窄  %扫描时长，距离/圆弧直径                 % total grid size [m]
x = 28e-3;  %纵向长度   深度aline                                % total grid size [m]%全角重建
sensor_angle  = 2* pi;         % [rad]
sensor_pos = [0e-3, 0e-3];    % [m]5

S1 = 1490; %
D1 = 1000; %双声速模型，探头内声速 单一声速% S2 = S1;% D2 = D1;
S2 = 1540;
D2 = 1000; %探头外组织声速

DBR_noise  = 0;       %DBR噪声
DBR_signal = 0;      %DBR光强初始信号

% trigdejit = 0;     %触发去抖
sensor_dejitter  = 0;
isdejitfig = 0;

phase_Decon = 0;             %去卷积运算

filter_low = 1;      %高通
w_low  = 0.4*1e6;   n1= 4;  % 高通 参数
filter_high = 1;     %低通
w_high = 60*1e6;  n2= 4;  % 低通 参数

Gaussfil = 1;        %去除光纤或者其他一圈不动的信号
gaussfil_rowstart = 1; %限定处理区域
gaussfil_rowsend = 1500;

Bscan1Dinter = 0;         %Bscan插值
L = 4000;            %一圈角度上想要插值的点数

ismedfilt = 1;              %中值滤波

arc_remove = 0;      %去除弧线信号
curve_num = [1,2,3,4];       %弧线编号

%----扣除光纤振荡强信号---
DBR_sig_remove = 1;
mask_length = 520;

%------delay光纤信号-----

singal_impair = 0;   %削弱图像中过强信号
im_value_532 =  2000;     %削弱阈值
im_value_1064 = 400;     %削弱阈值

DelayCut = 1;

bscan_view_orig = 0; %原始bscan%xlim([1601,2400]);ylim([150,250]);clim([-200,200])
b_Aline1 = 1;b_Aline2 = 12500;   %aline深度设置参数

pa_data_view = 0;%pa_data xlim([1601,2400]);ylim([1,51]);
ReconDepth = 6000; %实际重建aline深度，针对pa_data设置

c_1 = 800; %532nm Bscan图像colorbar
c_2 = 500; %1064nm Bscan图像colorbar2

Reconstruction = 1; %重建
c_3 = 4500; %532nm 重建图像colorbar %xlim([10,14]);ylim([-8,-4]);caxis([-5000,5000]);xlim([11.5,13.5]);ylim([-2,2.3]);set(figure(5),'position',[100,50,600,700]);
c_4 = 800; %1064nm 重建图像colorbar  %xlim([-0.5,0.7]);ylim([-12.55,-11.35]);
issave = 0;   %存储fig.png.mat

yn=1;

%% 读数据

%不同扫描轮次
FolderNum_str = cellstr(compose('%03d', FolderNum));
for reconround = 1:numel(FolderNum_str)
    CurrentFolderNum = FolderNum_str{reconround};
    FileLoadBase = cell(max(ChannelNum),1);
    matrix_comp_cell = cell(max(ChannelNum),1);
    
    % 读数据
    for cardIdx = ChannelNum
        FileloadArgc.FileNum = FileNum; FileloadArgc.depth = SampleDepth; FileloadArgc.Num_of_scan = Num_of_scan;
        FileName_normal = {'\Card1_ChA','\Card1_ChB','\Card2_ChA','\Card2_ChB','\Card3_ChA','\Card3_ChB','\Card4_ChA','\Card4_ChB'};
        FileLoadBase{cardIdx,1} = [FolderPath CurrentFolderNum FileName_normal{cardIdx} FileName_custom];
        matrix_comp_cell{cardIdx,1} = Func_8ch_DirecSample_SplitData(FileLoadBase{cardIdx,1},FileloadArgc);
    end
    n = max(cellfun(@numel, matrix_comp_cell));matrix_comp = cell(max(ChannelNum), n);
    for i = ChannelNum
        if ~isempty(matrix_comp_cell{i})
            matrix_comp(i, 1:numel(matrix_comp_cell{i})) = matrix_comp_cell{i};
        end
    end


    %% ---------------------------------------------------预处理-------------------------------------------------------------------------

    %选重建通道
    for Channelselect = ChannelNum

        % 两通道半径不一致，根据通道选择重建半径
        recon_radius = sensor_radius(find(ChannelNum == Channelselect));

        

        %多余触发滤除
        %考虑存在全扫情况，设计每轮触发数不一致，显式输入滤除多余触发
        % if isempty(Invalidtrigger)
        %     offset = 0;
        % else
        %     offset = Invalidtrigger(reconround);
        % end
        offset = size(matrix_comp{Channelselect},2) - TriggerNum;
for  recon_radius = [6.0:0.2:6.8].*1e-3

        %双波长切分
        temp = matrix_comp{Channelselect,Num_of_scan};
        temp(isnan(temp) | isinf(temp)) = 0;
        data_matrix{1,1} = double(temp(1:end,offset + 1:2:end));%532图像
        data_matrix{1,2} = double(temp(1:end,offset + 2:2:end));%1064图像
        data_matrix{1,2} = circshift(data_matrix{1,2},1,2);clear temp
        %分波长重建
        for fi_wave = 2:2%奇数1为532nm，偶数2为1064nm

            %双波长参数初始化
            if fi_wave == 1  %奇数1为532nm，偶数2为1064nm
                wave ='532';system_delay = sysDelay(find(ChannelNum == Channelselect),1);im_value = im_value_532;
                c_bscan = c_1;c_recon = c_3;
            else
                wave = '1064';system_delay = sysDelay(find(ChannelNum == Channelselect),2);im_value = im_value_1064;
                c_bscan = c_2;c_recon = c_4;
            end

            % 图像旋转
            switch CurrentFolderNum
                % case 13
                %     data_matrix{1,1} = circshift(data_matrix{1,1},-30,2);
            end

            % % % % %==================================================bscan插值=============================
            % if bscan_inter == 1
            %     data_matrix_inter = cell(1,2);
            %     data_matrix_temp = zeros(size(data_matrix{1,fi_wave},1) * 2,size(data_matrix{1,fi_wave},2));
            %     l_inter = linspace(1, size(data_matrix{1,fi_wave},1), size(data_matrix{1,fi_wave},1) * 2);
            %
            %     for i = 1:size(data_matrix{1,fi_wave},2)
            %         temp = data_matrix{1,fi_wave}(:,i);
            %         data_matrix_temp(:,i) = interp1(1:size(data_matrix{1,fi_wave},1),temp,l_inter);
            %     end
            %     data_matrix_inter{1,fi_wave} = data_matrix_temp;
            %     data_matrix{1,fi_wave} = data_matrix_inter{1,fi_wave};
            %     clear data_matrix_inter
            %     z = size(data_matrix{1,fi_wave},1);
            %     figure();imagesc(data_matrix{1,fi_wave});caxis([-c_bscan,c_bscan]);colorbar;
            %     titleName=strcat(CurrentFolderNum,'dat',',',num2str(wave),',',num2str(DBR),',','inter');title(titleName,'fontname','黑体','fontsize',15);
            %     set(gcf,'Windowstate','maximized');
            % end

            % %=================================================看rawBscan===================================================
            if bscan_view_orig==1
                b_rows = size(data_matrix{1,fi_wave},1);
                if b_rows < b_Aline2
                    figure;imagesc(data_matrix{1,fi_wave} (b_Aline1:end,:));clim([-c_bscan,c_bscan]);colorbar;
                                        % figure;imagesc(matrix_comp{Channelselect}(b_Aline1:end,1:20));clim([-c_bscan,c_bscan]);colorbar;
                    titleName=strcat(CurrentFolderNum,'dat',',','ch',num2str(Channelselect),32,num2str(wave),',','origin');title(titleName,'fontname','黑体','fontsize',15);
                    set(gcf,'Windowstate','maximized');
                    pause(1);
                else
                    figure;imagesc(data_matrix{1,fi_wave} (b_Aline1:b_Aline2,:));clim([-c_bscan,c_bscan]);colorbar;
                    titleName=strcat(CurrentFolderNum,'dat',',','ch',num2str(Channelselect),32,num2str(wave),',','origin');title(titleName,'fontname','黑体','fontsize',15);
                    set(gcf,'Windowstate','maximized');
                    pause(1);
                end
            else

                % % % % ==============================================================看DBR噪声=======================================================
                switch DBR_noise
                    case 1       %看DBR噪声
                        %看DBR noise
                        noise=  max(data_m(1:system_delay-50,:))-min(data_m(1:system_delay-50,:));
                        noise_mean=mean(noise);figure;plot(noise);
                        xlim([0,8000]); ylim([0,500]); title([CurrentFolderNum,'dat, noise mean=',num2str(noise_mean)]);

                        % % % % ==========================================================看DBR光强初始信号===================================================
                        switch DBR_signal
                            case 1     %看DBR光强初始信号
                                % %看DBR信号
                                dd=max(data_matrix{1,1}(system_delay-4:system_delay,:))-min(data_matrix{1,1}(system_delay-4:system_delay,:));
                                d_mean=mean(dd);
                                figure;plot(dd);ylim([0*1e4,20*1e4]);
                                titleName=strcat(CurrentFolderNum,'.dat-',num2str(wave),'-mean-',num2str(d_mean));title(titleName);
                        end
                end

                % % % % =========================================================去除光强光纤信号抖动 ================================================
                %% ================================组织信号抖动传感器位移估计====================================================
                % if trigdejit
                %     Bscan = data_matrix{1,fi_wave};
                %     refCol = 1001;
                %     Corrows = [161, 190];
                %     interpFactor = 10;
                %     Figpram.xylim = [501,1500,300,900];
                %     Figpram.clim = [-200,200];
                % 
                %     [aligned, shifts] = TrigDejitter(Bscan, refCol, Corrows, interpFactor,Figpram,0);
                %     data_matrix{1,fi_wave} = aligned;
                % end


                if  sensor_dejitter

                    % isdejitfig = 0;
                    dejitmode = 2;% 1二次对齐 2只对组织信号对齐 3只对dbr对齐

                    %%%%%%  =======================================================表面区域估计用于后续去抖==============================================================
                    bscanIn = data_matrix{1,1};
                    rowRange = [341, 365];
                    userParam = userParamcustomize0604;%自定义参数

                    surfacePrm = build_surface_dp_param_from_intra_param(userParam);%参数映射

                    [surfacePath, info] = Func_DetectSurfacePath_DP(bscanIn, rowRange, surfacePrm);
                    sufData.surfacePath = surfacePath;
                    sufData.info = info;

                    %%%%%%  =======================================================去除激光器触发带来的抖动运算==============================================================

                    % Fun_Jitter_Remove(data_matrix{1,k},system_delay,'max', 0);%古早去抖

                    simMeta.dx = stepsize;

                    if dejitmode ~= 2
                        temp_trig = data_matrix{1,fi_wave};
                        simMeta_trig.sound_speed = S1;
                        param_trig.IntraMinCorr = 0.6;
                        param_trig.IntraTrendWin = 21;
                        param_trig.IntraRowRange = [wave_params(fi_wave).delay - 10, wave_params(fi_wave).delay + 10];
                        param_trig.IntraMaxAdjacentShift = 7;
                        param_trig.IntraShiftLimit = 12;


                        % [estGrid_trig, result_trig,~] = Func_SensorJitterDejitter_EasyCopy(temp_trig, DAQ, simMeta_trig.sound_speed, simMeta.dx,1,param_trig);
                        [estGrid_trig, result_trig,~] = Func_SensorJitterDejitterv2(temp_trig, DAQ, simMeta_trig.sound_speed, simMeta.dx,1,param_trig);
                    end


                    if isdejitfig

                        % figure;imagesc(data_matrix{1,k});
                        % axis image;colorbar;
                        % xlim([2000,2600]);ylim([151,400]);
                        % clim([-200,200]);pause(0.1);

                        if dejitmode ~= 2
                            figure;imagesc(result_trig.previewBscan);
                            axis image;colorbar;
                            xlim([2000,2600]);ylim([151,400]);
                            clim([-200,200]);pause(0.1);
                        end

                    end

                    switch dejitmode
                        case 2
                            temp = data_matrix{1,fi_wave};%对信号对齐
                            temp(1:220,:) = 0;
                        otherwise
                            temp = result_trig.previewBscan;% 二次对齐
                    end

                    simMeta.sound_speed = S2;
                    % userParam = userParamcustomize;%自定义参数
                    % delaydiff = wave_params(k).delay - wave_params(1).delay;
                    userParam.IntraRowRange = rowRange ;%+ delaydiff


                    % result.peakRow = [];
                    [estGrid_sig, result, param] = Func_SensorDejitter_sufdetect(temp, DAQ, simMeta.sound_speed, simMeta.dx,1 ,userParam, sufData);

                    % [estGrid_sig2, result2, param2] = Func_SensorDejitter_sufdetect(result.previewBscan, DAQ, simMeta.sound_speed, simMeta.dx,1 ,userParam);%, sufData


                    % [estGrid_sig, result,~] = Func_SensorJitterDejitter_EasyCopy(temp, DAQ, simMeta.sound_speed, simMeta.dx,1,param);

                    if isdejitfig

                        for n = 1:1
                            figure;imagesc(temp);
                            % axis image;
                            % colorbar;
                            xlim([1, 2000]);ylim([241, 500]);
                            clim([-200,200]);pause(0.5);

                            figure;imagesc(result.previewBscan);
                            % axis image;
                            % colorbar;
                            xlim([1, 2000]);ylim([241, 500]);
                            clim([-200,200]);pause(0.5);
                            %
                            %             figure;imagesc(result2.previewBscan);
                            % % axis image;
                            % % colorbar;
                            % xlim([500, 1500]);ylim([601, 1800]);
                            % clim([-200,200]);pause(0.5);
                        end

                        if isfield(result, 'peakRow')
                            figure;
                            imagesc(temp);
                            % colormap gray;
                            % axis image;
                            hold on;

                            plot(result.peakRow, 'r-', 'LineWidth', 1.5);
                            plot(result.roiTop, 'b--', 'LineWidth', 1);
                            plot(result.roiBottom, 'b--', 'LineWidth', 1);

                            clim([-200,200]);
                            % axis image;
                            xlim([1, 2000]);ylim([241, 500]);
                            % xlim([2001, 4000]);ylim([601, 1800]);
                            % legend('surface ', 'ROI top', 'ROI bottom');
                            xlabel('A-lines');
                            ylabel('Depth sample');
                            title('curveROI');
                            pause(0.5);

                        end
                        % pause;
                        break
                    end


                    switch dejitmode
                        case 2
                            estGrid_trig = 0;
                        case 3
                            estGrid_sig = 0;
                    end

                    estGrid =   estGrid_trig + estGrid_sig;%

                end

                %%   % % % % ============================================================ 相位去卷积 ======================================================
                if phase_Decon == 1

                    ConfigArgc1.lengthdifferent = 2.68; %.68
                    ConfigArgc1.DAQ = DAQ;
                    ConfigArgc1.depth = SampleDepth;

                    % 测试臂长差
                    fiber_signal = data_matrix{1,fi_wave} (1:system_delay+101,701:1350);
                    % temp = fiber_signal;
                    % temp = abs(fft(temp));
                    % amp = mean(temp,'all');
                    %    估计臂长差
                    %     k = 1001;
                    %     signal = fiber_signal(1:end-1,k);
                    %     % % signal = mean(signal(:,k),2);
                    %     % % temp = mean(amp2(k));
                    %     fun_estimate_lengthdifferent(signal, ConfigArgc1.lengthdifferent, amp, DAQ);


                    % 光纤信号功率估计
                    %     temp = fiber_signal; % 光纤信号
                    temp = data_matrix{1,fi_wave} (251:2300,:); % 组织信号
                    temp = abs(fft(temp));
                    amp = smooth(mean(temp));
                    amp = mean(amp(:));
                    % 噪声功率估计
                    noise  = data_matrix{1,fi_wave} (21:120, :);
                    noise = noise(:);
                    fiber_signal_length = size(fiber_signal,1);
                    Nmax = floor( length(noise)/size(fiber_signal,1) ) * fiber_signal_length; % 计算需要多长的noise
                    noise = reshape(noise(1:Nmax),fiber_signal_length,[]);
                    noise_amp = abs(fft(noise));
                    noise_amp = mean(noise_amp(:));
                    %     noise_amp = rms(noise, "all");
                    % data_matrix{1,fi_wave}(data_matrix{1,fi_wave}>1000)=1000;
                    % 去掉DBR信号
                    data_matrix{1,fi_wave}(system_delay-33:system_delay+26,:)=data_matrix{1,fi_wave}(11:70,:);
                    % data_matrix{1,fi_wave}(system_delay-23:system_delay+76,:)=data_matrix{1,fi_wave}(11:110,:);

                    nsr = (noise_amp ./amp ).* ones(1,size(data_matrix{1,fi_wave} ,2));
                    data_matrix{1,fi_wave} = MainFun1DPhaseDeconv(data_matrix{1,fi_wave}, ConfigArgc1, 1);

                    % % 测试去卷积效果
                    %     signal = data_matrix(251:end,:);
                    %
                    %     is_plot = 0;
                    %     ConfigArgc1.depth = size(signal,1);
                    %
                    %
                    %     % 生成横坐标轴
                    %     N = ConfigArgc1.depth;
                    %     f_axis = (-N/2:N/2-1)*DAQ./N ; %[Hz]
                    %     f_axis = fftshift(f_axis); % 双边谱
                    %     if size(f_axis, 2) ~= 1 % 对齐before的行或列
                    %         f_axis = f_axis';
                    %     end

                    %      close all;
                    %     %     data_matrix = MainFun1Ddeconv(data_matrix, nsr, ConfigArgc1, 0); %
                    %         plot_times_freq(signal,f_axis,DAQ);sgtitle('before', 'FontSize', 16);
                    %
                    %     temp = MainFun1Ddeconv(signal, 0.0046.* ones(1,size(data_matrix,2)), ConfigArgc1, is_plot); %旧方法 rms
                    %     plot_times_freq(temp,f_axis,DAQ);sgtitle('noise rms', 'FontSize', 16);
                    %     temp = MainFun1Ddeconv(signal, 0.006.* ones(1,size(data_matrix,2)), ConfigArgc1, is_plot); %
                    %     plot_times_freq(temp,f_axis,DAQ);sgtitle('noise fft  / fiber fft', 'FontSize', 16);
                    %     temp = MainFun1Ddeconv(signal, nsr, ConfigArgc1, is_plot); %
                    %     plot_times_freq(temp,f_axis,DAQ);sgtitle('noise fft  / tiuse fft', 'FontSize', 16);
                    %
                end


                %%
                % % % %==============================================================去除光纤信号=====================================================

                %          processed_img = Freqfilter4FiberSignal(img, DAQ, lowpassfreq, coefficient, isplotH)

                % % % %==============================================================Filter 滤波器====================================================
                if filter_low==1
                    fiter_f =w_low;    %高通[Hz]
                    Wn = 2*fiter_f/DAQ ;
                    [B,A]=butter(n1,Wn,'high') ;
                    % data_matrix{1,fi_wave}((system_delay + 20):end,:) = filtfilt(B,A,data_matrix{1,fi_wave}((system_delay + 20):end,:));
                    % data_matrix{1,fi_wave}(301:end,:) = filtfilt(B,A,data_matrix{1,fi_wave}(301:end,:));
                    data_matrix{1,fi_wave} = filtfilt(B,A,data_matrix{1,fi_wave});

                end

                if filter_high==1
                    fiter_f = w_high;    %低通 [Hz]
                    Wn = 2*fiter_f/DAQ ;
                    [B,A]=butter(n2,Wn,'low') ;
                    % data_matrix{1,fi_wave}((system_delay + 20):end,:)  = filtfilt(B,A,data_matrix{1,fi_wave}((system_delay + 20):end,:));
                    data_matrix{1,fi_wave}  = filtfilt(B,A,data_matrix{1,fi_wave});

                end

                % figure;imagesc(data_matrix{1,fi_wave});clim([-500,500]);
                % if filter_high == 1 || filter_low == 1
                %     title(CurrentFolderNum, '0.7-40M带通');
                % else
                %     title('未滤波');
                % end

                %% 空间滤波
                if Gaussfil == 1

                    gaussfil_pre = data_matrix{1,fi_wave}(gaussfil_rowstart:gaussfil_rowsend,:);
                    gaussfil_after  = Freqfilter4FiberSignal(gaussfil_pre , DAQ, 1e6, 45e6, 0.75,0);
                    data_matrix{1,fi_wave}(gaussfil_rowstart:gaussfil_rowsend,:) = gaussfil_after;

                end

                % figure;imagesc(data_matrix{1,fi_wave});clim([-500,500]);
                % if Gaussfil == 1
                %     title(CurrentFolderNum, '横向空间滤波');
                % else
                %     title('未空间滤波');
                % end
                thetafil = 0;
               if thetafil == 1
                length_per_step = 20e-6;
                isplotH = 0;
                theta = 43;
                figure;
               Fun_bscan_k_visualisation(data_matrix{1,fi_wave}, DAQ, length_per_step);
               data_matrix{1,fi_wave} = Freqfilter4AngleButter(data_matrix{1,fi_wave}, theta, DAQ,length_per_step , S1, 'high', 0, isplotH);
               figure;
               Fun_bscan_k_visualisation(data_matrix{1,fi_wave}, DAQ, length_per_step);
               end 
               % figure;imagesc(data_matrix{1,fi_wave});clim([-500,500]);
               %  if Gaussfil == 1
               %      title(CurrentFolderNum, '角度空间滤波');
               %  else
               %      title('未空间滤波');
               %  end
 
                % % % %==================================================================插值=========================================================
               
                if Bscan1Dinter == 1
                    x_original = linspace(1,Alines,Alines);
                    x_new = linspace(1,Alines,L);
                    data_matrix_2=zeros(zz,L);
                    for i = 1:zz
                        data_matrix_2(i, :) = pchip(x_original, data_matrix{1,fi_wave} (i, :), x_new);
                    end
                    data_matrix{1,fi_wave} =data_matrix_2;
                    [zz, Alines]=size(data_matrix{1,fi_wave} );
                end

                % % % %================================================================中值滤波=======================================================
                if ismedfilt==1
                    data_matrix{1,fi_wave} = medfilt2(data_matrix{1,fi_wave}, [3 3]) ;
                end

                % figure;imagesc(data_matrix{1,fi_wave});clim([-500,500]);
                % if ismedfilt==1
                %     title(CurrentFolderNum,'中值滤波');
                % else
                %     title('未中值滤波');
                % end


                % % % %=================================================去除弧线信号=================================
                if arc_remove == 1
                    arc_img = data_matrix{1,fi_wave};
                    for c_num = curve_num
                        load(fullfile(FolderPath,'\arc\',sprintf('%d_arc_inter_%d.mat', CurrentFolderNum , c_num)));


                        arc_region = ArcsufPadded(arc_img , arc_suf_dual);
                        data_matrix{1,fi_wave} = data_matrix{1,fi_wave} .* arc_region{1,fi_wave};
                    end
                end
                % % % %=========================================================直接抠掉光纤本身的震荡信号=============================================

                %                     data_matrix{1,fi_wave}(system_delay-30+1:system_delay+120,:) = repmat(data_matrix_noise(1:50,:),3,1);
                if DBR_sig_remove == 1
                    %                         rp_times = 5;
                    %                         noise_mask_orig = data_matrix_noise(1:50,:);
                    %                         noise_mask = repmat(noise_mask_orig,rp_times,1);
                    %                         mask_length = size(noise_mask,1);
                    %                         data_matrix{1,fi_wave}(1:mask_length,:) = noise_mask;
                    switch fi_wave
                        case 1
                            data_matrix{1,fi_wave}(1:mask_length,:) = 0;
                        case 2
                            data_matrix{1,fi_wave}(1:mask_length + (sysDelay(2) - sysDelay(1)),:) = 0;

                    end
                end

                % % % %==========================================================全图信号设置阈值削减=================================================
                if singal_impair == 1

                    data_matrix{1,fi_wave} (data_matrix{1,fi_wave}  >  im_value) =  im_value;
                    data_matrix{1,fi_wave} (data_matrix{1,fi_wave}  < -im_value) = -im_value;


                end

                % % % %==================================================================Delay========================================================

                if DelayCut == 1
                    data_matrix{1,fi_wave}=data_matrix{1,fi_wave}(system_delay:end,:);

                end

                [zz, Alines]=size(data_matrix{1,fi_wave} );

                      % figure;imagesc(data_matrix{1,fi_wave} (b_Aline1:b_Aline2,:));clim([-c_bscan,c_bscan]);colorbar;


                % % % %============================================================根据重建步长，抽值=================================================
                data_depth = size(data_matrix{1,fi_wave},1);
                if ReconDepth > data_depth
                    pa_data = data_matrix{1,fi_wave} (1:end,(yn-1)*Alines+1:space_n:yn*Alines);
                else
                    pa_data = data_matrix{1,fi_wave} (1:ReconDepth-system_delay,(yn-1)*Alines+1:space_n:yn*Alines);%
                end
                % % % %=====================预处理后bscan===============================================

                if pa_data_view == 1
                    figure;imagesc(pa_data(1:end,:));clim([-c_bscan,c_bscan]);colorbar;
                    titleName=strcat(CurrentFolderNum,'dat',',',num2str(wave));title(titleName,'fontname','黑体','fontsize',15);
                    set(gcf,'Windowstate','maximized');
                else

                % % % %================================================================是否要重建=====================================================

                    if Reconstruction==1
                        % % % %================================================================重建设置=======================================================

                        [Nsample, Nstep] = size(pa_data);
                        num_sensor_points = Nstep;
                        dx = stepsize;    dy =  stepsize; % [m]
                        Nx = ceil(x/dx);  Ny = ceil(y/dy);
                        kgrid_recon = kWaveGrid(Nx, dx, Ny, dy);

                        cart_sensor_mask = makeCartCircle(recon_radius, num_sensor_points, sensor_pos, sensor_angle);
                        % cart_sensor_mask = makeCartCircle(sensor_radius, num_sensor_points, sensor_pos, sensor_angle);
                        % sensor_radius_grid_points = round(sensor_radius / kgrid_recon.dx);
                        % binary_sensor_mask = makeCircle(kgrid_recon.Nx, kgrid_recon.Ny, kgrid_recon.Nx/2 + 1, kgrid_recon.Ny/2 + 1, sensor_radius_grid_points, sensor_angle);
                        % sensor.mask = binary_sensor_mask;

                        % % % % % =============================================================================================
                        %生成传感器矩阵
                        if sensor_dejitter == 1

                            [~,cart_sensor_mask_corr_full] = sensorpos_dejitter(cart_sensor_mask,estGrid(1:space_n:end),sensor_pos,kgrid_recon);
                            [binary_sensor_mask_corr, p_binary] = MaskCart2Binary(cart_sensor_mask, cart_sensor_mask_corr_full, pa_data, kgrid_recon );

                            sensor.mask = binary_sensor_mask_corr;
                            sensor.time_reversal_boundary_data = p_binary;

                        else

                            % binary_sensor_mask = makeCircle(kgrid_recon.Nx, kgrid_recon.Ny, kgrid_recon.Nx/2 + 1, kgrid_recon.Ny/2 + 1, sensor_radius_grid_points, sensor_angle);
                            binary_sensor_mask = cart2grid(kgrid_recon, cart_sensor_mask );
                            sensor_data = pa_data';

                            sensor.mask = binary_sensor_mask;
                            sensor.time_reversal_boundary_data = interpCartData(kgrid_recon, sensor_data, cart_sensor_mask, binary_sensor_mask);

                        end
                        % % % %===============================================实际传感器点数与num_sensor_points强制对齐数据===================================
                        %                     if c ~= num_sensor_points
                        %                         % 调整数据维度
                        %                         if c < size(pa_data, 2)
                        %                             pa_data = pa_data(:, 1:c);          % 截断
                        %                         else
                        %                             pa_data = [pa_data, zeros(Nsample, c - size(pa_data, 2))];  % 补零
                        %                         end
                        %                         % 更新传感器点数
                        %                         num_sensor_points = c;
                        %                     end

                        % % % %=================================================================双声速============================================== ==========
                        %S1 探头内水环境的声速和密度
                        tissue_radius =r_water;  tissue_pos = [0e-3, 0e-3]; tissue_angle=-2*pi;
                        cx=Nx/2;cy=Ny/2;
                        water_circle= makeDisc(Nx, Ny,cx,cy,tissue_radius/dx);
                        % figure;imagesc(water_circle);axis image;colorbar;

                        % % % 全局介质的密度和声速S2
                        medium.sound_speed = S2 * ones(Nx, Ny);   % [m/s]
                        medium.density = D2 * ones(Nx, Ny);       % [kg/m^3]
                        medium.sound_speed = medium.sound_speed - (S2-S1)*water_circle;
                        medium.density = medium.density - (D2-D1)*water_circle;
                        %figure;imagesc(medium.sound_speed);axis image;colorbar;

                        % % % %==================================================================重建=========================================================
                        dt = 1/DAQ;   % [s]
                        Nt = Nsample;
                        kgrid_recon.setTime(Nt,dt);

                        source.p0 = 0;
                        % sensor_data = pa_data';
                        % sensor.time_reversal_boundary_data = interpCartData(kgrid_recon, sensor_data, cart_sensor_mask, binary_sensor_mask);

                        % sensor.time_reversal_boundary_data = interpCartData(kgrid_recon, pa_data', cart_sensor_mask,circle);
                        input_args = {'Smooth', false, 'PMLInside', false, 'PlotPML', false,'PlotSim',false};%,'DataCast','double'
                        p0_recon = kspaceFirstOrder2DG(kgrid_recon, medium, source, sensor, input_args{:});
                        p0_recon = double(p0_recon);

                        % % % %================================================================扣中心的圆=====================================================
                        center = [0, 0];  % 圆心的坐标 [x, y]
                        radius_0 =r_0.*Nx./x;  % 探头装置圆的半径
                        circle_mask_0 = makeDisc(Nx, Ny, center(1), center(2), radius_0);%生成一个大小为 Nx × Ny 的二值掩膜矩阵，其中圆形区域为 1，背景为 0
                        circle_mask_0=(circle_mask_0 -1)*(-1);
                        % figure;imagesc(circle_mask_0);
                        p0_recon_0=p0_recon.*circle_mask_0;

                        % % % %================================================================Fig图像========================================================
                        %      temp = abs(hilbert(p0_recon_0));

                        switch sensor_angle
                            case pi
                                rotang = -2;
                            case 2*pi
                                rotang = -1;
                        end

                        % figure;imagesc(kgrid_recon.y_vec * 1e3, kgrid_recon.x_vec * 1e3, flip(rot90(p0_recon_0,2),2) );pause(0.1);
                        figure;imagesc(kgrid_recon.y_vec * 1e3, kgrid_recon.x_vec * 1e3, p0_recon_0);
                        colormap(getColorMap);ylabel('x(mm)');xlabel('y(mm)');axis image;colorbar;colormap(gray);clim([-c_recon,c_recon]);
                        titleName=strcat(CurrentFolderNum,',',num2str(wave),',R=',num2str(recon_radius*1E3),',space=',num2str(space_n),...
                            ',step=',num2str(stepsize*1E6),',S2=',num2str(S2));
                        title(titleName,'fontsize',11,'FontName','黑体'); set(gcf,'Windowstate','maximized');pause(1);

                        % xlim([-14,14]);ylim([-14,14]);
                        % xlim([-0.75,0.1]);ylim([8.8,9.6]);


                        % % % %================================================================存储数据=======================================================
                        if issave == 1
                            %        set(gcf, 'Color', 'none'); % 图形背景透明
                            %        set(gca, 'Color', 'none'); % 坐标轴背景透明
                            %        legend('boxoff'); % 图例背景透明

                            % 保存路径
                            currentTime = datevec(datestr(now));
                            currentSec = ([num2str(currentTime(:,4)),num2str(currentTime(:,5)),num2str(currentTime(:,6))]);
                            mkdir([FolderPath,'recon_fig\',num2str(Channelselect)]);%在该路径下创建文件夹为1的文件 %1限阈值 %4 %6为180度重建
                            FolderPath_save = fullfile(FolderPath,'recon_fig');
                            folderPath = fullfile(num2str(FolderPath_save), '\', num2str(Channelselect));
                            fileName = fullfile(folderPath, [CurrentFolderNum,'_',num2str(wave),'nm_',num2str(ChannelNum),'_','R',num2str(recon_radius*1000),'mm_',...
                                '_speed_sound',num2str(S2),'Re_Dp',num2str(ReconDepth),'_',num2str(currentSec)]);
        

                            % % % ----------------------------存储fig文件--------------------------------
                            fileName_fig = fullfile([fileName,'.fig']);
                            savefig(gcf, fileName_fig);

                            % % %--------------------------- 存储mat文件---------------------------------
                            data_sensor_init.kgrid_recon=kgrid_recon;
                            data_sensor_init.S2=S2;
                            data_sensor_init.p0_recon=p0_recon;
                            data_sensor_init.p0_recon_0=p0_recon_0;
                            data_sensor_init.recon_radius=recon_radius;
                            data_sensor_init.space_n=space_n;
                            data_sensor_init.stepsize=stepsize;
                            data_sensor_init.num_sensor_points=num_sensor_points;
                            data_sensor_init.sensor_angle=sensor_angle;
                            fileName_mat = fullfile([fileName,'.mat']);
                            save(fileName_mat, 'data_sensor_init');

                            % % %------------------------ 存储高分辨率png文件----------------------------
                            fileName_png = fullfile([fileName,'.fig']);
                            print(gcf,fileName_png,'-dpng','-r400');

                            % % %--------------------显示图像时，恢复透明操作----------------------------
                            %        set(gca, 'Color', 'white'); % 坐标轴背景为白色（不透明）
                            %        set(gcf, 'Color', 'white'); % 图形窗口背景为白色（不透明）
                            set(gcf,'Windowstate','maximized');

                        end
                    end
                end
            end
        end
    end
end
end


%% ----------------------------------------------滤除光纤信号
% if IsFilterLine == 1
%     img=pa_data0;
%     f2=fftshift(fft2(img));
%     D0=3;
%     [M,N]=size(f2);
%     m=floor(M/2);n=floor(N/2);%(n,m)为中心点
%     for i=1:M
%         for j=1:N
%             D=sqrt((i-m)^2+(j-n)^2);
%             if j>n-D0 && j<n+D0
%                 H(i,j)=exp(-1/2*D^2/D0^2);
%             else
%                 H(i,j)=1;
%             end
%             if i>m-D0 && i<m+D0
%                 H(i,j)=exp(-1/2*D^2/D0^2);
%             end
%         end
%     end
%     f3=f2.*H;
%     img3 = (ifft2(ifftshift(f3)));
%     img4 = (real(img3));%real函数表示留下复数的实部
%     pa_data0 = img4;
% end
%
%
%
%
%
%

%%  FFT 看方向角
% %
% % figure;imagesc(p0_recon_0);
% % % % % % %
% leaf_f_in=  data_sensor_init.p0_recon_0;%插值，包络后 2um后数据
% figure;imagesc(leaf_f_in);colormap(gray);caxis([0,15000]);
%
% leaf_f=leaf_f_in(601:1600,3201:4000)';
% % leaf_f=leaf_f_in(6600:7600,12000:13000);
% %  figure;imagesc(leaf_f);colormap(gray);caxis([0,15000]);
% % % % % % %
% m=size(leaf_f,1);
% n=size(leaf_f,2);
% dmouse=data_sensor_init.stepsize*1e3./(size(leaf_f,1)./size(leaf_f_in,1));
%
% %Angle spec
% stepsize = data_sensor_init.stepsize;
% v = 1496;
% sDAQ = 1/(stepsize/v);
% fm= (-m/2:m/2-1).*sDAQ/m/1e6;
% fn= (-n/2:n/2-1).*sDAQ/n/1e6;
%
% % km = (-m/2:m/2-1).*sDAQ/m./v./1e6;
% % kn = (-n/2:n/2-1).*sDAQ/n./v./1e6;
%
% mouse_ang = abs(fftshift(fft2(leaf_f)));
% mouse_ang = mouse_ang./max(max(mouse_ang));
% mouse_ang_log = 20*log10(mouse_ang);
%
% figure();imagesc(fn,fm,mouse_ang_log);
% % set(gcf,'unit','centimeters','position',[10,10,10,10]);
% % imagesc(kn,km,mouse_ang_log);
% % axis image;
% caxis([-100,-30]);set(gca,'position',[0.15,0.15,0.65,0.75]);
% colormap(gray);colorbar;
% axis image;
% % xlim([-0.0267,0.0267]);ylim([-0.0267,0.0267]);
% set(gca,'FontName', 'Arial', 'FontSize',12);
% % set(gca,'xTick',[-0.02 -0.01 0 0.01 0.02]);
% % set(gca,'yTick',[-0.02 -0.01 0 0.01 0.02]);
% % xlabel('km (\mum^-^1)','FontName', 'Arial', 'FontSize',16);
% % ylabel('kn (\mum^-^1)','FontName', 'Arial', 'FontSize',16);%% FFT2
% xlabel('fm (MHz)','FontName', 'Arial', 'FontSize',16);
% ylabel('fn (MHz)','FontName', 'Arial', 'FontSize',16);%% FFT2
%
%





%% 看光纤信号谱
% single_Spectrum = fft_signal(data_matrix,220,DAQ,2000);



function single_side_nor = fft_signal(data,N_start,N_end,DAQ)


fft_dbr_single = abs(fft(data(N_start:N_end,:)));
N= N_end-N_start+1;
fft_normalized = fft_dbr_single / N;


N_half = floor(N/2)+1;
single_side = fft_normalized(1:N_half, :);
single_side(2:end-1, :) = 2 * single_side(2:end-1, :);


data_fft = mean(single_side,2);
% single_side_nor = mapminmax(data_fft,0,1);
single_side_nor = (max(data_fft,[],2)-min(data_fft))/(max(data_fft)-min(data_fft));


f_axis_MHz = (0:N_half-1) * DAQ / N /1e6;

figure();plot(f_axis_MHz, single_side_nor);
xlabel('Frequency (MHz)');
ylabel('Normalized Amplitude');
title('Single-Sided Amplitude Spectrum');

figure();plot(f_axis_MHz, data_fft);
xlabel('Frequency (MHz)');
ylabel('Normalized Amplitude');
title('Single-Sided Amplitude Spectrum');

% figure();plot(f_axis_MHz, single_side(:, x));



end

%% 看信号谱
function single_side_nor = fft_signal_aline(data,DAQ,c_recon_2)

fig = figure('Name','区域选择','NumberTitle', 'off');
ax = axes(fig);
imagesc(data);
clim([-c_recon_2,c_recon_2]);
% caxis([0,c_recon_2]);
axis image;colorbar;colormap("parula"); axis equal tight;
set(gcf,'Windowstate','maximized');

[rows, cols] = size(data);

[x1(1), y1(1), button1] = ginput(1);
if button1 == 3
    close(fig);
end
[x1(2), y1(2), button1] = ginput(1);
if button1 == 3
    close(fig);
end
close(fig);
% 处理第一步选择
col_idx1 = round([x1(1), x1(2)]);
row_idx1 = round([y1(1), y1(2)]);
col_idx1 = max(1, min(cols, col_idx1));
row_idx1 = max(1, min(rows, row_idx1));

cols_start = min(col_idx1);
cols_end = max(col_idx1);
rows_start = min(row_idx1);
rows_end = max(row_idx1);

fft_dbr_single = abs(fft(data(rows_start:rows_end,cols_start:cols_end)));
N = rows_end-rows_start+1;
fft_normalized = fft_dbr_single / N;


N_half = floor(N/2)+1;
single_side = fft_normalized(1:N_half, :);
single_side(2:end-1, :) = 2 * single_side(2:end-1, :);


data_fft = mean(single_side,2);
% single_side_nor = mapminmax(data_fft,0,1);
single_side_nor = (max(data_fft,[],2)-min(data_fft))/(max(data_fft)-min(data_fft));


f_axis_MHz = (0:N_half-1) * DAQ / N /1e6;

figure();plot(f_axis_MHz, single_side_nor);
xlabel('Frequency (MHz)');
ylabel('Normalized Amplitude');
title('Single-Sided Amplitude Spectrum');

figure();plot(f_axis_MHz, data_fft);
xlabel('Frequency (MHz)');
ylabel('Normalized Amplitude');
title('Single-Sided Amplitude Spectrum');

% figure();plot(f_axis_MHz, single_side(:, x));



end


%% 滤除光纤信号的函数
function processed_img = Freqfilter4FiberSignal(img, fs, lowpassfreq, highpassfreq, coefficient, isplotH)
% 频域高斯带阻滤波函数（向量化实现）
% 输入参数：
% img : 原始图像矩阵（二维灰度图像）
% D0 : 滤波带宽参数（控制高斯衰减区域宽度）
% 输出参数：
% processed_img : 滤波后图像
D0 = lowpassfreq/fs;
D1 = highpassfreq/fs;
if nargin < 4
    coefficient = 0.75;
    if coefficient >= 1 || coefficient <= 0
        coefficient = 0.75;
        warring('coefficient 需要在（0，1）')
    end
end
if nargin <5
    isplotH = 0;
end
% ========== 频域变换 ==========
img = double(img);
img = fftshift(fft2(img));
% ========== 向量化滤波器设计 ==========
[M, N] = size(img);
fm = (-M/2:M/2-1)./M ;
fn = (-N/2:N/2-1)./N ;
% 生成网格坐标（向量化核心）
[I, J] = meshgrid(fn, fm); % 生成行列坐标矩阵[6,7](@ref)
distance = sqrt(I.^2 + J.^2); % 计算所有点到中心的距离[2,5](@ref)
% 构建水平/垂直带状区域逻辑矩阵
% horizontal_band = (abs(J) > D1); % X方向带状区域[2](@ref)
vertical_band = (abs(I) < D0); % Y方向带状区域[2](@ref)
% 合并区域并计算高斯衰减
H = ones(M, N);
% band_mask = horizontal_band | vertical_band; % 十字形区域[1](@ref)
band_mask = vertical_band; % 仅取y方向滤波，去除横向不变值
% H(band_mask) = exp(-1 * coefficient * distance(band_mask).^2 / D0^2); % 向量化指数计算[3](@ref)
H(band_mask) = 0;
% figure;    imagesc((abs(H)));colormap jet; colorbar;title('二维频谱分布');axis image;

% ========== 频域滤波及逆变换 ==========
f3 = img .* H;
img3 = ifft2(ifftshift(f3));
processed_img = real(img3);
if isplotH == true
    % 显示二维灰度图
    figure;
    subplot(1,3,1);
    imagesc((abs(img)));
    colormap jet; colorbar;
    title('滤波前');
    axis image;
    subplot(1,3,2);
    imagesc((abs(H)));
    colormap jet; colorbar;
    title('二维频谱分布');
    axis image;
    subplot(1,3,3);
    imagesc((abs(f3)));
    colormap jet; colorbar;
    title('滤波后');
    axis image;
end
end


%% 估算臂长差的函数
function fun_estimate_lengthdifferent(signal, length_different,amp,DAQ)

% 输入：
% signal: 光打在光纤上的信号
% length_different：臂长差 unit：m
% amp： 预估的信号强度
% DAQ 采样率

N2 = length(signal);
signal_spectrum_measured = smooth(abs(fft(signal)));
ff = (1:N2)'/N2*DAQ;
f_half = ff(1:length(ff)/2+1);
% 构建臂长差带来的传递函数
signal_spectrum_ideal = calculate_transfer_function(length_different, f_half','real');% 单边谱
% 构建NI采集的低通滤波器
H = fun_generateDAQsystemTF(f_half', DAQ);
% H = abs(freqz(b, a, f_half', fs));% 计算指定频率点的频率响应
signal_spectrum_ideal = 1.4*amp*signal_spectrum_ideal.*H ;
sigma = amp*0.1;
signal_spectrum_ideal = sqrt(signal_spectrum_ideal.^2 + sigma.^2);
signal_spectrum_ideal = [signal_spectrum_ideal conj(signal_spectrum_ideal(end-1:-1:2))]; % 转换为双边谱

% 画图 估算臂长差
figure;set(gcf,'Position',[50,50,500,500]);
plot(ff*1e-6,signal_spectrum_ideal);
hold on;
plot(ff*1e-6,signal_spectrum_measured);
title(['signal spectrum,length different=' num2str(length_different),'m']);
ylabel('amplitude');xlabel('freq(MHz)');
% ylim([0 1e5]);
xlim([0 100]);
% ylim([0 1e4]);
legend('simulation','measured');



%% 生成系统的传递函数
    function H = fun_generateDAQsystemTF(axis_f, DAQ)
        %% 生成系统的传递函数
        % 输入
        % DAQ：采样率 unit:Hz
        % axis_f : f的坐标轴
        % 输出
        % H：频率的滤波器
        fs = DAQ;fl = 45e6;
        Wn = fl / (fs/2);% 计算归一化截止频率
        n = 1;
        [b, a] = butter(n, Wn, 'low');% 设计巴特沃斯低通滤波器
        H = abs(freqz(b, a, axis_f, fs));% 计算指定频率点的频率响应

    end
end

%%
function plot_times_freq(y,ff,DAQ)
% 画图

if size(y,2) == 1
    y = y ./ max(abs(y),[],"all"); % 归一化
    y_f = fft(y);
else
    y_f = fft(y);
    y_f = mean(y_f,2);
    y = ifft(y_f);
    y =  y ./ max(abs(y),[],"all"); % 归一化
    y_f = fft(y);
end
figure;set(gcf,'Position',[50,50,500,1000]);

tt = (1:length(y))/DAQ; % unit:s
% 时域谱
subplot(2,1,1);
plot(tt *1e6, smooth(y));

xlabel('Times(\mus)');
xlim([0.1 1.5]);title('Times');
% ylim([-1.8e4 1e4]);
ylim([-1.05 1.05]);

% 幅度谱
subplot(2,1,2);
plot(fftshift(ff*1e-6), fftshift(smooth(abs(y_f))));title('Frequency');hold on;
ylabel('amplitude');xlabel('freq(MHz)');
% ylim([0 1e5]);
xlim([0 100]);ylim([0 200]);
end

function [after] = MainFun1DPhaseDeconv(before, ConfigArgc, is_plot)
%UNTITLED4 此处显示有关此函数的摘要
%   此处显示详细说明

DAQ = ConfigArgc.DAQ;
length_different = ConfigArgc.lengthdifferent;


if nargin < 4
    is_plot = 0;
end


% amp =   7.1350e+04 ; % 计算信号强度


% 生成横坐标轴
N = ConfigArgc.depth;
% if mod(N, 2) == 0
%     % N 为偶数
%     f_axis = (-N/2 : N/2-1) * DAQ / N;
% else
%     % N 为奇数
%     f_axis = (-(N-1)/2 : (N-1)/2) * DAQ / N;
% end
% f_axis =fftshift(fftshift(f_axis)) ; % 双边谱
f_axis = generate_fft_freq_direct(N, DAQ);

if size(f_axis, 2) ~= 1 % 对齐before的行或列
    f_axis = f_axis';
end



% % signal = signal .* hamming(length(signal));
% % 估算臂长差
% length_different = 3; % 臂长差
% amp =  mean(abs(fft(signal)));

% fun_estimate_lengthdifferent(signal, length_different,amp,DAQ)
H = fun_generateDAQsystemTF(f_axis, DAQ);
% 理想光声信号
% wave_width = 7e-9; % 脉宽 unit:s % 200Hz的时5.8ns
% ideal_PA_signal = idealPA(DAQ, ConfigArgc.depth, wave_width, is_plot); %
% 臂长差的传递函数 correlated self-heterodyne (COSH) method
TF_cosh = calculate_transfer_function(length_different, f_axis,'plural'); % 传递函数的频谱
% sigma = 1;
% TF_cosh = TF_cosh .* fun_balance(TF_cosh, sigma);

spect = TF_cosh; % 传递函数的频谱
% spect = H .*TF_cosh; % 传递函数的频谱
psf = ifft(spect,'symmetric');
psf = circshift(psf, round(N/2)); % 将脉冲信号平移至前面

after = zeros(size(before, 1), size(before, 2));

for i = 1:size(before, 2)
    % after(:,i) = PACT_deconvwnr(before(:,i), psf, nsr(i));
    after(:,i) = phase_filter4fiber_sensor(before(:,i), psf);
end
% ================= 可视化 =================
if is_plot == true && min(size(before)) == 1
    plot_times_freq_1(before,f_axis,DAQ);sgtitle('before', 'FontSize', 16);
    plot_times_freq_1(after,f_axis,DAQ);sgtitle('after', 'FontSize', 16);
end




end

%% 生成系统的传递函数
function H = fun_generateDAQsystemTF(axis_f, DAQ)
%% 生成系统的传递函数
% 输入
% DAQ：采样率 unit:Hz
% axis_f : f的坐标轴
% 输出
% H：频率的滤波器
fs = DAQ;
if fs == 200e6 % 200 MHz 采样率
    fl = 80e6; n = 3;
else  % 100 MHz 采样率
    fl = 45e6; n = 1;
end
Wn = fl / (fs/2);% 计算归一化截止频率
[b, a] = butter(n, Wn, 'low');% 设计巴特沃斯低通滤波器
H = abs(freqz(b, a, axis_f, fs));% 计算指定频率点的频率响应

end

function f_axis = generate_fft_freq_direct(N, DAQ)
% 生成直接对应fft(y)结果的频率轴（未fftshift）
% fft(y)的结果顺序：[0, 正频率, 负频率]

if mod(N, 2) == 0
    % N为偶数
    % 顺序：0, 1, 2, ..., N/2-1, -N/2, ..., -2, -1
    f_axis = [0:N/2-1, -N/2:-1] * (DAQ/N);
else
    % N为奇数
    % 顺序：0, 1, 2, ..., (N-1)/2, -(N-1)/2, ..., -2, -1
    f_axis = [0:(N-1)/2, -(N-1)/2:-1] * (DAQ/N);
end
end

function b = fun_balance(y, sigma)
%BALANCE  将 y==0 替换为极小值后计算 (y^2+sigma^2)^0.5/y
%
%   b = BALANCE(y)        默认 sigma = 1e-8
%   b = BALANCE(y, sigma) 用户指定 sigma
%
%   输入:
%       y      : 实数向量/矩阵
%       sigma  : 可选正则化参数（标量，建议 1e-8~1e-6）
%   输出:
%       b      : 与 y 同尺寸的结果

if nargin < 2
    sigma = 1e-8;          % 默认正则化参数
end

% 把 y==0 替换成极小值，避免后续除零
y = y + (y == 0) * 1e-12;

% 计算平衡函数
b = sqrt(y.^2 + sigma^2) ./ y;
end

function output = phase_filter4fiber_sensor(intput_img, psf)
%% 构建相位滤波器并滤波
% 输入信号图片intput_img
% 输入对应角度的PSF
% 输出滤波后的图片output
h_f = build_phase(fft2(psf)); % 计算滤波器对应的频谱
p0_h = real(ifft2(h_f)); % 计算滤波器的空间分布
output = conv2(intput_img, p0_h,'same'); % 信号与滤波器卷积

%% 构建取卷积的矩阵
    function h_freq = build_phase(psf_freq)
        % 输入  psf的频谱
        % 输出 滤波器的频谱
        % 因为精度原因，小数点后10位的结果有误差
        % ifft直接取实部即可
        % amp = abs(psf_freq);
        amp = 1; % 强度
        ang = 1 * angle(psf_freq) ; % 相位
        % ang =   unwrap(angle(psf_freq)) ;
        h_freq = amp.* exp( -1i .* ang); % 根据强度与相位计算滤波器的频谱
        % a = amp.* cos(ang) + 1i * amp.* sin(ang);
        % a = real(psf_freq) - 1i * imag(psf_freq);
    end
end


%% 弧线区赋值


function arc_region = ArcsufPadded(arc_img , arc_suf_dual)

p_ceil = 10;
p_floor = 25;

[a_rows,a_cols] = size(arc_img);
padded_mask_532(1:a_rows,1:a_cols) = 1;
padded_mask_1064 = padded_mask_532;

for p_cols = arc_suf_dual{1,1}(1,1):arc_suf_dual{1,1}(1,end)

    p_rows_532 = arc_suf_dual{1,1}(2,p_cols - arc_suf_dual{1,1}(1,1) +1);
    p_rows_1064 = arc_suf_dual{1,2}(2,p_cols - arc_suf_dual{1,2}(1,1) +1);

    padded_mask_532 (p_rows_532  - p_ceil : p_rows_532  + p_floor , p_cols) = 0;
    padded_mask_1064(p_rows_1064 - p_ceil : p_rows_1064 + p_floor , p_cols) = 0;

end

arc_region{1,1} = padded_mask_532;
arc_region{1,2} = padded_mask_1064;



end




function [matrix_comp] = Func_8ch_DirecSample_SplitData(FileLoad,FileloadArgc)

Num_of_scan = FileloadArgc.Num_of_scan;
FileNum = FileloadArgc.FileNum;
TotalDepth =  FileloadArgc.depth;    %depth

% open *.dat,read binary file %%
cFile = [];
if isfield(FileloadArgc,'FileInx')
    fileIndices = FileloadArgc.FileInx;
else
    fileIndices = 1:FileNum;
end
for i = fileIndices
    if i <= 10
        FileID = ['00' num2str(i-1)];
    else
        FileID = ['0' num2str(i-1)];
    end
    FolderPath = ResolveDirectSampleFilePath(FileLoad, FileID);
    if isempty(FolderPath)
        if isempty(cFile)
            error('No data file matched: %s%s.dat', FileLoad, FileID);
        else
            break;
        end
    end
    fid = fopen(FolderPath, 'r');
    if fid == -1,error('文件打开失败');end
    cFiletemp = fread(fid,inf,'uint16=>uint16');
    cFiletemp = halfprecision_to_double(cFiletemp);
    cFile = [cFile;single(cFiletemp)];
end

fclose('all');
%换算成rad
% cFile = cFile.*1000.*2.*pi./DAQ;

% reshape arry %%
if isfield(FileloadArgc,'ScanInx')
    scanIdx = FileloadArgc.ScanInx;
else
    scanIdx = 1;
end

if isfield(FileloadArgc, 'move_Bscan')
    move_Bscan = FileloadArgc.move_Bscan;

    Inxshift1 = (scanIdx-1) * move_Bscan * TotalDepth + 1;
    NeedSamples = TotalDepth * move_Bscan * Num_of_scan;

    if size(cFile,1) >= (Inxshift1 + NeedSamples - 1)
        cFile = cFile(Inxshift1:Inxshift1+NeedSamples-1);
    else
        if size(cFile,1) >= Inxshift1
            cFile = cFile(Inxshift1:end);
        else
            cFile = [];
        end
        cFile = [cFile;zeros(NeedSamples-size(cFile,1),1)];
    end
    raw_data = reshape(cFile,TotalDepth,[]);

    raw_matrix  = reshape(raw_data,TotalDepth,move_Bscan,[]);

else

    raw_matrix = reshape(cFile,TotalDepth,[]);

end

raw_matrix = -1.*raw_matrix; %相位取反

matrix_comp = cell(Num_of_scan,1);
for i=1:Num_of_scan
    if Num_of_scan == 1
        matrix_comp{i,1} = raw_matrix;
    else
        matrix_comp{i,1} = raw_matrix(:,:,i);
    end

end

end



function out = halfprecision_to_double(h)
h = uint16(h);

sign_bit = bitshift(bitand(h, hex2dec('8000')), -15);
exp_bits  = bitshift(bitand(h, hex2dec('7C00')), -10);
frac_bits = bitand(h, hex2dec('03FF'));

out = zeros(size(h), 'double');
s = 1 - 2*double(sign_bit);

idx_sub = (exp_bits == 0);
out(idx_sub) = s(idx_sub) .* 2.^(-14) .* (double(frac_bits(idx_sub)) / 1024);

idx_norm = (exp_bits > 0 & exp_bits < 31);
out(idx_norm) = s(idx_norm) .* 2.^(double(exp_bits(idx_norm)) - 15) .* ...
    (1 + double(frac_bits(idx_norm)) / 1024);

idx_inf = (exp_bits == 31 & frac_bits == 0);
out(idx_inf) = s(idx_inf) .* Inf;

idx_nan = (exp_bits == 31 & frac_bits ~= 0);
out(idx_nan) = NaN;
end

function filePath = ResolveDirectSampleFilePath(fileLoad, fileId)
pattern = [fileLoad fileId '.dat'];
matches = dir(pattern);
if isempty(matches)
    filePath = '';
    return;
end
[~, order] = sort({matches.name});
matches = matches(order);
filePath = fullfile(matches(1).folder, matches(1).name);
if numel(matches) > 1
    warning('Multiple data files matched "%s"; using "%s".', pattern, filePath);
end
end

%% k 域角度巴特沃斯滤波：沿 theta 方向做 N 阶高通/低通
%% k 域角度滤波：N 阶巴特沃斯渐变，N=0 时退化为硬边
function processed_img = Freqfilter4AngleButter(img, theta, DAQ, length_per_step, S1, filtertype, N, isplotH)
% 输入参数：
%   img             待处理数据（行=时间/深度采样点，列=扫描位置/阵元）
%   theta           截止角 [deg]，从横轴（ky 轴、扫描方向）向上度量
%   DAQ             采集卡采样率 [Hz]
%   length_per_step 扫描步长 [m]
%   S1              声速 [m/s]
%   filtertype      可选，'high'（默认，滤除贴横轴的小角度分量）或 'low'（滤除大角度分量）
%   N               可选，阶数，默认 5。N=0 时不做渐变，直接按角度硬切
%                   （N 在本函数里是阶数，列数记为 Ncol）
%   isplotH         可选，=1 时画滤波前频谱、滤波器 H、滤波后频谱；默认 0
% 输出参数：
%   processed_img   滤波后的 B-scan
% 说明：
%   滤波变量是每个频点到横轴的夹角 phi = atan2d(|kx|,|ky|)，phi 取 0~90 度；
%   N>=1：'high' 用 H = 1/sqrt(1+(theta/phi)^(2N))，'low' 用 H = 1/sqrt(1+(phi/theta)^(2N))；
%         两者在 phi=theta 处都是 -3 dB，N 越大过渡越陡、硬边振铃越小。
%   N=0 ：'high' 用 H = (phi>=theta)，'low' 用 H = (phi<=theta)，即矩形响应。
%         其中 'low' 的判据与 Freqfilter4Angle 完全相同，结果一致；'high' 是它的补集。
%   'high' 等价于保留绕竖轴（深度方向）±(90-theta) 的楔形。

if nargin < 6 || isempty(filtertype)
    filtertype = 'high';
end
if nargin < 7 || isempty(N)
    N = 0;
end
if nargin < 8
    isplotH = 0;
end
assert(theta > 0 && theta < 90, 'theta 必须取 0 到 90 度之间。');

% ========== 频域变换 ==========
img = double(img);
img = fftshift(fft2(img));

% ========== 两个方向的等效频率轴 ==========
[M, Ncol] = size(img);
fm = (-M/2:M/2-1)./M;                    % 行（时间/深度）方向，周期/采样点
fn = (-Ncol/2:Ncol/2-1)./Ncol;           % 列（扫描位置）方向，周期/阵元
[I, J] = meshgrid(fn, fm);               % I 沿扫描方向，J 沿深度方向

kx = J*DAQ*1e-6;                         %[MHz] 深度/时间方向
ky = I*S1/length_per_step*1e-6;          %[MHz] 扫描方向

% ========== 沿 theta 方向的响应 ==========
phi = atan2d(abs(kx), abs(ky));          %[deg] 到横轴的夹角，0~90
if strcmpi(filtertype, 'high')
    if N == 0
        H = double(phi >= theta);            % 硬边，滤除 phi<theta
    else
        H = 1./sqrt(1 + (theta./phi).^(2*N)); % 滤除 phi<theta
    end
else
    if N == 0
        H = double(phi <= theta);            % 硬边，滤除 phi>theta
    else
        H = 1./sqrt(1 + (phi./theta).^(2*N)); % 滤除 phi>theta
    end
end

% ========== 频域滤波及逆变换 ==========
f3 = img .* H;
img3 = ifft2(ifftshift(f3));
processed_img = real(img3);

if isplotH == true
      % 显示二维灰度图
    figure; 
    subplot(1,3,1);
    imagesc((abs(img))); 
    colormap jet; colorbar;
    title('滤波前');
    axis image;
    subplot(1,3,2);
    imagesc((abs(H))); 
    colormap jet; colorbar;
    title('二维频谱分布');
    axis image;
    subplot(1,3,3);
    imagesc((abs(f3))); 
    colormap jet; colorbar;
    title('滤波后');
    axis image;
end
end
function Fun_bscan_k_visualisation(temp, DAQ, length_per_step)
% B-scan k 域可视化：行=时间/深度采样点，列=扫描位置
% DAQ             采集卡采样率 [Hz]
% length_per_step 扫描步长 [m]
% S1              声速 [m/s]，用于把扫描步长折算成等效频率
    S1 = 1500;
    [m,n] = size(temp);
    SpaceDAQ = S1/(length_per_step);   %[Hz] 扫描方向等效采样率
    kx = (-m/2:m/2-1)*DAQ./m *1e-6;         %[MHz] 行：时间/深度方向
    ky = (-n/2:n/2-1)*SpaceDAQ./n *1e-6;    %[MHz] 列：扫描方向

    temp_f = abs(fftshift(fft2(temp)));
    % figure;
    % set(gcf,'Position',[350,80,1000*1.5,450*1.5],'color','w');
    imagesc(ky, kx, temp_f);
    maxvalue = 8e6;
    clim([0,maxvalue]);
    axis normal;
    axis([ -40 40 -70 70]);
    colormap(gray);
    colorbar;
    ylabel('Frequency [MHz]');
    xlabel('Frequency [MHz]');
end