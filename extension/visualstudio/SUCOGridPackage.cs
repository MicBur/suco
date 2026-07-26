using System;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.VisualStudio.Shell;
using Task = System.Threading.Tasks.Task;

namespace SUCOGrid.VSIX
{
    [PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
    [Guid("4A12F5B6-8C3D-4F2E-9D10-3E4B5C6A7D8E")]
    [ProvideOptionPage(typeof(SUCOOptionPage), "SUCO Grid", "General", 0, 0, true)]
    public sealed class SUCOGridPackage : AsyncPackage
    {
        protected override async Task InitializeAsync(CancellationToken cancellationToken, IProgress<ServiceProgressData> progress)
        {
            await this.JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        }
    }
}
