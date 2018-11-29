gcc = csvread('gcc.csv');
gobmk = csvread('gobmk.csv');
hmmer = csvread('hmmer.csv');
mcf = csvread('mcf.csv');

gccrob = gcc(1:end/2,:);
gobmkrob = gobmk(1:end/2,:);
hmmerrob = hmmer(1:end/2,:);
mcfrob = mcf(1:end/2,:);

gcccpr = gcc(end/2+1:end,:);
gobmkcpr = gobmk(end/2+1:end,:);
hmmercpr = hmmer(end/2+1:end,:);
mcfcpr = mcf(end/2+1:end,:);

gcc95 = sortrows(gcc(gcc(:, 1) >= 0.95 * max(gcc(:, 1)), :),2);
gobmk95 = sortrows(gobmk(gobmk(:, 1) >= 0.95 * max(gobmk(:, 1)), :),2);
hmmer95 = sortrows(hmmer(hmmer(:, 1) >= 0.95 * max(hmmer(:, 1)), :),2);
mcf95 = sortrows(mcf(mcf(:, 1) >= 0.95 * max(mcf(:, 1)), :),2);

gccbest = gcc95(1,:);
gobmkbest = gobmk95(1,:);
hmmerbest = hmmer95(1,:);
mcfbest = mcf95(1,:);